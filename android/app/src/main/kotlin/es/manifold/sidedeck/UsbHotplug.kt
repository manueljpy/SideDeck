package es.manifold.sidedeck

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import io.flutter.plugin.common.EventChannel

/**
 * Notifies Flutter when a USB output with at least 4 channels appears or
 * disappears. [AudioDeviceCallback] already runs on the main looper.
 */
class UsbHotplug(context: Context) : EventChannel.StreamHandler {
    private val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val handler = Handler(Looper.getMainLooper())
    private var sink: EventChannel.EventSink? = null
    private var registered = false

    private val callback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) {
            for (d in added) {
                if (!UsbDevices.isUsbOutput(d)) continue
                val channels = UsbDevices.maxChannels(am, d)
                if (channels < UsbDevices.MIN_MIXER_CHANNELS) continue
                emit(UsbDevices.payload(d, channels, "attached"))
            }
        }

        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) {
            for (d in removed) {
                if (UsbDevices.isUsbOutput(d)) emit(UsbDevices.payload(d, 0, "detached"))
            }
        }
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        sink = events
        if (registered) return
        // Immediately invokes the callback with devices already connected.
        am.registerAudioDeviceCallback(callback, handler)
        registered = true
    }

    override fun onCancel(arguments: Any?) {
        sink = null
        stop()
    }

    fun stop() {
        if (!registered) return
        am.unregisterAudioDeviceCallback(callback)
        registered = false
    }

    private fun emit(payload: Map<String, Any>) {
        sink?.success(payload)
    }
}
