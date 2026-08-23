package es.manifold.sidedeck

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        keepScreenOn()
    }

    override fun onResume() {
        super.onResume()
        keepScreenOn()
    }

    override fun onFlutterUiDisplayed() {
        super.onFlutterUiDisplayed()
        keepScreenOn()
    }

    private fun keepScreenOn() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.decorView.keepScreenOn = true
        findViewById<android.view.View>(android.R.id.content)?.keepScreenOn = true
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "sidedeck/audio")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "usbOutputDevice" -> result.success(findUsbOutput(this))
                    "startUsbPlayback" -> {
                        val handle = (call.argument<Number>("handle") ?: 0).toLong()
                        val deviceId = call.argument<Int>("deviceId") ?: 0
                        val channels = call.argument<Int>("channels") ?: 4
                        result.success(UsbPlayer.start(this, handle, deviceId, channels))
                    }
                    "stopUsbPlayback" -> {
                        UsbPlayer.stop()
                        result.success(null)
                    }
                    else -> result.notImplemented()
                }
            }
    }

    override fun onDestroy() {
        UsbPlayer.stop()
        super.onDestroy()
    }

    /** Android 14+ reports the configurations a USB output stream can be opened with. */
    private fun mixerAttributesOf(am: AudioManager, device: AudioDeviceInfo): String {
        if (Build.VERSION.SDK_INT < 34) return ""
        return try {
            am.getSupportedMixerAttributes(device).joinToString("; ") {
                "${it.format.channelCount}ch enc=${it.format.encoding} sr=${it.format.sampleRate}"
            }
        } catch (_: Exception) {
            ""
        }
    }

    private fun findUsbOutput(context: Context): Map<String, Any> {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val devices = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        var best: AudioDeviceInfo? = null
        var bestCh = 0
        for (d in devices) {
            val counts = d.channelCounts
            val maxCh = if (counts.isNotEmpty()) counts.max() else 0
            val usb = d.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                d.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
                d.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
            if (Log.isLoggable("sidedeck", Log.DEBUG)) {
                Log.i(
                    "sidedeck",
                    "audio out id=${d.id} type=${d.type} name=${d.productName} " +
                        "maxCh=$maxCh counts=[${counts.joinToString()}] " +
                        "indexMasks=[${d.channelIndexMasks.joinToString { "0x" + Integer.toHexString(it) }}] " +
                        "rates=[${d.sampleRates.joinToString()}] usb=$usb " +
                        "mixer=[${mixerAttributesOf(am, d)}]",
                )
            }
            if (!usb) continue
            val preferDevice = d.type == AudioDeviceInfo.TYPE_USB_DEVICE &&
                best?.type != AudioDeviceInfo.TYPE_USB_DEVICE
            if (maxCh > bestCh || (maxCh == bestCh && preferDevice)) {
                best = d
                bestCh = maxCh
            }
        }
        return mapOf(
            "id" to (best?.id ?: 0),
            "channels" to bestCh,
            "name" to (best?.productName?.toString() ?: ""),
        )
    }
}
