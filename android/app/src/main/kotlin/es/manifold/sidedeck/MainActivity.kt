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
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private var usbHotplug: UsbHotplug? = null

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
        val messenger = flutterEngine.dartExecutor.binaryMessenger
        MethodChannel(messenger, "sidedeck/audio")
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
        val hotplug = UsbHotplug(this)
        usbHotplug = hotplug
        EventChannel(messenger, "sidedeck/usb").setStreamHandler(hotplug)
    }

    override fun onDestroy() {
        usbHotplug?.stop()
        usbHotplug = null
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
        if (Log.isLoggable("sidedeck", Log.DEBUG)) {
            for (d in am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                Log.d(
                    "sidedeck",
                    "audio out id=${d.id} type=${d.type} name=${d.productName} " +
                        "maxCh=${UsbDevices.maxChannels(am, d)} " +
                        "counts=[${d.channelCounts.joinToString()}] " +
                        "indexMasks=[${d.channelIndexMasks.joinToString { "0x" + Integer.toHexString(it) }}] " +
                        "rates=[${d.sampleRates.joinToString()}] usb=${UsbDevices.isUsbOutput(d)} " +
                        "mixer=[${mixerAttributesOf(am, d)}]",
                )
            }
        }
        val best = UsbDevices.findBest(am)
        return mapOf(
            "id" to (best?.first?.id ?: 0),
            "channels" to (best?.second ?: 0),
            "name" to (best?.first?.productName?.toString() ?: ""),
        )
    }
}
