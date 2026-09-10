import 'dart:io';

import 'package:flutter/services.dart';

class UsbOutputInfo {
  const UsbOutputInfo({required this.id, required this.channels, required this.name});

  final int id;
  final int channels;
  final String name;
}

class UsbStartResult {
  const UsbStartResult({
    required this.ok,
    required this.channels,
    required this.routedId,
    required this.routedName,
    required this.error,
  });

  final bool ok;
  final int channels;
  final int routedId;
  final String routedName;
  final String error;
}

class UsbHotplugEvent {
  const UsbHotplugEvent({
    required this.attached,
    required this.id,
    required this.channels,
    required this.name,
  });

  final bool attached;
  final int id;
  final int channels;
  final String name;
}

class UsbMixerOffer {
  const UsbMixerOffer({required this.id, required this.channels, required this.name});

  final int id;
  final int channels;
  final String name;

  String get label => UsbOutput.deviceLabel(name);
}

class UsbOutput {
  static const _ch = MethodChannel('sidedeck/audio');
  static const _events = EventChannel('sidedeck/usb');

  /// Android names USB outputs like "USB-Audio - EP-136"; show just the hardware.
  static String deviceLabel(String name) {
    final trimmed = name.split(' - ').last.trim();
    return trimmed.isEmpty ? 'mixer' : trimmed;
  }

  static Stream<UsbHotplugEvent>? _hotplug;

  static Stream<UsbHotplugEvent> events() {
    if (!Platform.isAndroid) return const Stream.empty();
    return _hotplug ??= _events.receiveBroadcastStream().map((raw) {
      final map = raw is Map ? raw : const {};
      return UsbHotplugEvent(
        attached: map['event'] == 'attached',
        id: (map['id'] as num?)?.toInt() ?? 0,
        channels: (map['channels'] as num?)?.toInt() ?? 0,
        name: '${map['name'] ?? ''}',
      );
    });
  }

  static Future<UsbOutputInfo?> find() async {
    try {
      final raw = await _ch.invokeMethod<dynamic>('usbOutputDevice');
      if (raw is! Map) return null;
      final id = (raw['id'] as num?)?.toInt() ?? 0;
      if (id <= 0) return null;
      return UsbOutputInfo(
        id: id,
        channels: (raw['channels'] as num?)?.toInt() ?? 0,
        name: '${raw['name'] ?? ''}',
      );
    } catch (_) {
      return null;
    }
  }

  static Future<UsbStartResult> startPlayback({
    required int engineHandle,
    required int deviceId,
    int channels = 4,
  }) async {
    try {
      final raw = await _ch.invokeMethod<dynamic>('startUsbPlayback', {
        'handle': engineHandle,
        'deviceId': deviceId,
        'channels': channels,
      });
      if (raw is! Map) {
        return const UsbStartResult(
          ok: false,
          channels: 0,
          routedId: 0,
          routedName: '',
          error: 'No response from Android audio',
        );
      }
      return UsbStartResult(
        ok: raw['ok'] == true,
        channels: (raw['channels'] as num?)?.toInt() ?? 0,
        routedId: (raw['routedId'] as num?)?.toInt() ?? 0,
        routedName: '${raw['routedName'] ?? ''}',
        error: '${raw['error'] ?? ''}',
      );
    } catch (e) {
      return UsbStartResult(
        ok: false,
        channels: 0,
        routedId: 0,
        routedName: '',
        error: '$e',
      );
    }
  }

  static Future<void> stopPlayback() async {
    try {
      await _ch.invokeMethod<void>('stopUsbPlayback');
    } catch (_) {}
  }
}
