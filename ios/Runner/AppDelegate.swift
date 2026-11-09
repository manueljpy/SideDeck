import AVFoundation
import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  private var usbHandler: UsbRouteHandler?

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    UIApplication.shared.isIdleTimerDisabled = true
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
    UIApplication.shared.isIdleTimerDisabled = true
    guard let registrar = engineBridge.pluginRegistry.registrar(forPlugin: "sidedeck/audio") else {
      return
    }
    setupChannels(registrar.messenger())
  }

  private func setupChannels(_ messenger: FlutterBinaryMessenger) {
    let usb = UsbRouteHandler()
    usbHandler = usb
    FlutterMethodChannel(name: "sidedeck/audio", binaryMessenger: messenger)
      .setMethodCallHandler { call, result in
        switch call.method {
        case "usbOutputDevice":
          result(Self.usbOutput())
        case "accessPath":
          let path = (call.arguments as? String) ?? ""
          result(URL(fileURLWithPath: path).startAccessingSecurityScopedResource())
        case "startUsbPlayback", "stopUsbPlayback":
          result(nil)
        default:
          result(FlutterMethodNotImplemented)
        }
      }
    FlutterEventChannel(name: "sidedeck/usb", binaryMessenger: messenger)
      .setStreamHandler(usb)
  }

  static func usbOutput() -> [String: Any] {
    let session = AVAudioSession.sharedInstance()
    for output in session.currentRoute.outputs where output.portType == .usbAudio {
      return [
        "id": stableId(output.uid),
        "channels": session.maximumOutputNumberOfChannels,
        "name": output.portName,
      ]
    }
    return ["id": 0, "channels": 0, "name": ""]
  }

  static func stableId(_ uid: String) -> Int {
    var hash = 0
    for scalar in uid.unicodeScalars {
      hash = (hash &* 31 &+ Int(scalar.value)) & 0x7fff_ffff
    }
    return hash == 0 ? 1 : hash
  }
}

final class UsbRouteHandler: NSObject, FlutterStreamHandler {
  private var sink: FlutterEventSink?
  private var observer: NSObjectProtocol?
  private var lastId = 0

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink)
    -> FlutterError?
  {
    sink = events
    emitCurrent()
    observer = NotificationCenter.default.addObserver(
      forName: AVAudioSession.routeChangeNotification,
      object: nil,
      queue: .main
    ) { [weak self] _ in
      self?.emitCurrent()
    }
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    sink = nil
    if let observer {
      NotificationCenter.default.removeObserver(observer)
    }
    observer = nil
    return nil
  }

  private func emitCurrent() {
    let info = AppDelegate.usbOutput()
    let id = info["id"] as? Int ?? 0
    let channels = info["channels"] as? Int ?? 0
    if id > 0 && channels >= 4 {
      if lastId == id {
        return
      }
      lastId = id
      sink?([
        "event": "attached",
        "id": id,
        "channels": channels,
        "name": info["name"] as? String ?? "",
      ])
    } else if lastId != 0 {
      let detached = lastId
      lastId = 0
      sink?([
        "event": "detached",
        "id": detached,
        "channels": 0,
        "name": "",
      ])
    }
  }
}
