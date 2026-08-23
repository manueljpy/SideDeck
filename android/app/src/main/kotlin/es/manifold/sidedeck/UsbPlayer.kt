package es.manifold.sidedeck

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioMixerAttributes
import android.media.AudioTrack
import android.os.Build
import android.util.Log
import androidx.annotation.RequiresApi
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max

/**
 * Multi-channel USB playback: deck A on USB 1-2, deck B on USB 3-4.
 *
 * Two paths, best first:
 *  1. Android 14+ preferred mixer attributes, which reconfigure the USB output
 *     stream itself. Without this the stream is usually opened as stereo and
 *     AudioFlinger silently drops channels 3-4.
 *  2. A plain AudioTrack with a channel index mask, which is the only other
 *     Android API that addresses USB channels by number rather than by speaker
 *     position (Oboe/AAudio treat a 4ch device as QUAD and drive the front pair).
 */
object UsbPlayer {
    init {
        System.loadLibrary("dj_engine")
    }

    @JvmStatic
    external fun usbRender(handle: Long, buffer: ByteBuffer, frames: Int, channels: Int)

    private const val FRAMES = 256
    private const val SAMPLE_RATE = 48000

    @Volatile
    private var running = false
    private var thread: Thread? = null
    private var track: AudioTrack? = null
    private var audioManager: AudioManager? = null
    private var preferredDevice: AudioDeviceInfo? = null
    private var preferredAttributes: AudioAttributes? = null

    private fun mediaAttributes() = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .build()

    @Synchronized
    fun start(context: Context, handle: Long, deviceId: Int, channels: Int): Map<String, Any> {
        stop()
        val notes = ArrayList<String>()
        if (handle == 0L) return failure("Engine not ready", notes)

        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager = am
        val device = findDevice(am, deviceId)
        if (device == null) notes.add("no USB output device in AudioManager")

        var t: AudioTrack? = null
        if (Build.VERSION.SDK_INT >= 34 && device != null) {
            t = openViaMixerAttributes(am, device, notes)
        } else if (device != null) {
            notes.add("Android ${Build.VERSION.SDK_INT}: preferred mixer attributes need API 34")
        }
        if (t == null) t = openViaDirectTrack(device, channels, notes)
        if (t == null) return failure("Android refused a multi-channel USB track", notes)

        track = t
        t.play()
        val actual = t.channelCount
        val routed = t.routedDevice
        val encoding = t.audioFormat
        running = true
        thread = Thread({ loop(handle, actual, encoding) }, "sidedeck-usb").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
        notes.add("track: ${actual}ch ${encodingName(encoding)} @${t.sampleRate}Hz")
        notes.add("routed to: ${routed?.productName ?: "unknown"} (type ${routed?.type ?: -1})")
        Log.i("sidedeck", "usb start: ${notes.joinToString(" | ")}")

        val ok = actual >= 4
        return mapOf(
            "ok" to ok,
            "channels" to actual,
            "routedName" to (routed?.productName?.toString() ?: ""),
            "error" to if (ok) "" else "Only $actual channels available over USB",
        )
    }

    @Synchronized
    fun stop() {
        running = false
        thread?.join(500)
        thread = null
        try {
            track?.pause()
            track?.flush()
            track?.stop()
        } catch (_: Exception) {
        }
        track?.release()
        track = null
        clearPreferred()
    }

    private fun clearPreferred() {
        val am = audioManager
        val device = preferredDevice
        val attrs = preferredAttributes
        preferredDevice = null
        preferredAttributes = null
        if (Build.VERSION.SDK_INT < 34 || am == null || device == null || attrs == null) return
        try {
            am.clearPreferredMixerAttributes(attrs, device)
        } catch (e: Exception) {
            Log.w("sidedeck", "clearPreferredMixerAttributes: $e")
        }
    }

    private fun failure(message: String, notes: List<String>): Map<String, Any> {
        Log.e("sidedeck", "usb start failed: $message | ${notes.joinToString(" | ")}")
        return mapOf(
            "ok" to false,
            "channels" to 0,
            "routedName" to "",
            "error" to message,
        )
    }

    private fun findDevice(am: AudioManager, deviceId: Int): AudioDeviceInfo? {
        val outputs = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        return outputs.firstOrNull { it.id == deviceId }
            ?: outputs.filter { isUsb(it) }.maxByOrNull { it.channelCounts.maxOrNull() ?: 0 }
    }

    private fun isUsb(d: AudioDeviceInfo) = d.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
        d.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
        d.type == AudioDeviceInfo.TYPE_USB_ACCESSORY

    @RequiresApi(34)
    private fun openViaMixerAttributes(
        am: AudioManager,
        device: AudioDeviceInfo,
        notes: MutableList<String>,
    ): AudioTrack? {
        val supported = try {
            am.getSupportedMixerAttributes(device)
        } catch (e: Exception) {
            notes.add("getSupportedMixerAttributes failed: $e")
            return null
        }
        if (supported.isEmpty()) {
            notes.add("device offers no mixer attributes")
            return null
        }
        for (m in supported) notes.add("offered: ${describe(m.format)} behavior=${m.mixerBehavior}")

        val multi = supported.filter { it.format.channelCount >= 4 }
        if (multi.isEmpty()) {
            notes.add("device offers no >=4ch mixer attribute")
            return null
        }
        val pick = multi.filter { isSupportedEncoding(it.format.encoding) }.maxByOrNull { score(it.format) }
        if (pick == null) {
            notes.add("4ch offered but encodings unsupported: ${multi.joinToString { encodingName(it.format.encoding) }}")
            return null
        }

        val attrs = mediaAttributes()
        val applied = try {
            am.setPreferredMixerAttributes(attrs, device, pick)
        } catch (e: Exception) {
            notes.add("setPreferredMixerAttributes threw: $e")
            false
        }
        if (!applied) {
            notes.add("setPreferredMixerAttributes rejected")
            return null
        }
        preferredDevice = device
        preferredAttributes = attrs

        val t = build(attrs, pick.format, device)
        if (t == null) {
            notes.add("track build failed for ${describe(pick.format)}")
            clearPreferred()
            return null
        }
        notes.add("forced USB mixer to ${describe(pick.format)}")
        return t
    }

    /**
     * Fallback when the mixer cannot be reconfigured. Devices that report no channel
     * index masks (the EP-136 is one) only accept a channel position mask, where QUAD
     * lays out as front L/R then back L/R, i.e. USB 1-2 then USB 3-4.
     */
    private fun openViaDirectTrack(
        device: AudioDeviceInfo?,
        channels: Int,
        notes: MutableList<String>,
    ): AudioTrack? {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val encodings = intArrayOf(
            AudioFormat.ENCODING_PCM_32BIT,
            AudioFormat.ENCODING_PCM_FLOAT,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        val hasIndexMasks = device?.channelIndexMasks?.isNotEmpty() == true
        val counts = linkedSetOf(if (channels >= 8) 8 else 4, 4, 8)

        val candidates = ArrayList<AudioFormat>()
        fun addPositionQuad() {
            for (e in encodings) {
                candidates.add(
                    AudioFormat.Builder()
                        .setEncoding(e)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_QUAD)
                        .build(),
                )
            }
        }
        fun addIndexMasks() {
            for (ch in counts) {
                for (e in encodings) {
                    candidates.add(
                        AudioFormat.Builder()
                            .setEncoding(e)
                            .setSampleRate(SAMPLE_RATE)
                            .setChannelIndexMask((1 shl ch) - 1)
                            .build(),
                    )
                }
            }
        }
        if (hasIndexMasks) {
            addIndexMasks()
            addPositionQuad()
        } else {
            notes.add("device reports no channel index masks; using position masks")
            addPositionQuad()
            addIndexMasks()
        }

        for (format in candidates) {
            val t = build(attrs, format, device)
            if (t != null && t.channelCount >= 4) {
                notes.add("direct track: ${describe(format)}")
                return t
            }
            t?.release()
        }
        notes.add("no 4ch track configuration was accepted")
        return null
    }

    private fun build(
        attrs: AudioAttributes,
        format: AudioFormat,
        device: AudioDeviceInfo?,
    ): AudioTrack? {
        val rate = if (format.sampleRate > 0) format.sampleRate else SAMPLE_RATE
        val bytesPerFrame = format.channelCount * bytesPerSample(format.encoding)
        val bufBytes = max(bytesPerFrame * (rate / 20), bytesPerFrame * FRAMES * 4)
        return try {
            val builder = AudioTrack.Builder()
                .setAudioAttributes(attrs)
                .setAudioFormat(format)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(bufBytes)
            if (Build.VERSION.SDK_INT >= 26) {
                builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_NONE)
            }
            val t = builder.build()
            if (t.state != AudioTrack.STATE_INITIALIZED) {
                t.release()
                return null
            }
            if (device != null) t.preferredDevice = device
            t
        } catch (e: Exception) {
            Log.e("sidedeck", "track build ${describe(format)}: $e")
            null
        }
    }

    private fun loop(handle: Long, channels: Int, encoding: Int) {
        val t = track ?: return
        val samples = FRAMES * channels
        // The engine always renders float; convert in place where the device wants integers.
        val buffer = ByteBuffer.allocateDirect(samples * 4).order(ByteOrder.nativeOrder())
        val floats = buffer.asFloatBuffer()
        val ints = buffer.asIntBuffer()
        val shorts = if (encoding == AudioFormat.ENCODING_PCM_16BIT) ShortArray(samples) else null
        while (running) {
            usbRender(handle, buffer, FRAMES, channels)
            val written = when (encoding) {
                AudioFormat.ENCODING_PCM_FLOAT -> {
                    buffer.position(0)
                    t.write(buffer, samples * 4, AudioTrack.WRITE_BLOCKING)
                }
                AudioFormat.ENCODING_PCM_32BIT -> {
                    for (i in 0 until samples) {
                        val v = floats.get(i).coerceIn(-1f, 1f).toDouble()
                        ints.put(i, (v * 2147483647.0).toInt())
                    }
                    buffer.position(0)
                    t.write(buffer, samples * 4, AudioTrack.WRITE_BLOCKING)
                }
                else -> {
                    val s = shorts!!
                    for (i in 0 until samples) {
                        s[i] = (floats.get(i) * 32767f).toInt().coerceIn(-32767, 32767).toShort()
                    }
                    t.write(s, 0, samples, AudioTrack.WRITE_BLOCKING)
                }
            }
            if (written < 0) {
                Log.e("sidedeck", "usb write error $written")
                break
            }
        }
    }

    private fun isSupportedEncoding(encoding: Int) =
        encoding == AudioFormat.ENCODING_PCM_FLOAT ||
            encoding == AudioFormat.ENCODING_PCM_32BIT ||
            encoding == AudioFormat.ENCODING_PCM_16BIT

    private fun bytesPerSample(encoding: Int) =
        if (encoding == AudioFormat.ENCODING_PCM_16BIT) 2 else 4

    private fun encodingName(encoding: Int) = when (encoding) {
        AudioFormat.ENCODING_PCM_16BIT -> "pcm16"
        AudioFormat.ENCODING_PCM_FLOAT -> "float"
        AudioFormat.ENCODING_PCM_24BIT_PACKED -> "pcm24packed"
        AudioFormat.ENCODING_PCM_32BIT -> "pcm32"
        else -> "enc$encoding"
    }

    private fun score(format: AudioFormat): Int {
        var s = format.channelCount * 1000
        if (format.sampleRate == SAMPLE_RATE) s += 100
        s += when (format.encoding) {
            AudioFormat.ENCODING_PCM_FLOAT -> 30
            AudioFormat.ENCODING_PCM_32BIT -> 20
            else -> 10
        }
        return s
    }

    private fun describe(format: AudioFormat): String {
        val index = if (Build.VERSION.SDK_INT >= 23) format.channelIndexMask else 0
        return "${format.channelCount}ch ${encodingName(format.encoding)} sr=${format.sampleRate} " +
            "mask=0x${Integer.toHexString(format.channelMask)} idx=0x${Integer.toHexString(index)}"
    }
}
