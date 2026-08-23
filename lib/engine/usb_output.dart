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
    required this.routedName,
    required this.error,
  });

  final bool ok;
  final int channels;
  final String routedName;
  final String error;
}

class UsbOutput {
  static const _ch = MethodChannel('sidedeck/audio');

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
          routedName: '',
          error: 'No response from Android audio',
        );
      }
      return UsbStartResult(
        ok: raw['ok'] == true,
        channels: (raw['channels'] as num?)?.toInt() ?? 0,
        routedName: '${raw['routedName'] ?? ''}',
        error: '${raw['error'] ?? ''}',
      );
    } catch (e) {
      return UsbStartResult(ok: false, channels: 0, routedName: '', error: '$e');
    }
  }

  static Future<void> stopPlayback() async {
    try {
      await _ch.invokeMethod<void>('stopUsbPlayback');
    } catch (_) {}
  }
}
