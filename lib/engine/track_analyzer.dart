import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

class TrackAnalysis {
  const TrackAnalysis({
    required this.bpm,
    required this.key,
    required this.beatOffset,
  });

  final double bpm;
  final int key;
  final double beatOffset;
}

typedef _AnalyzeC = Int32 Function(
  Pointer<Utf8>,
  Pointer<Float>,
  Pointer<Int32>,
  Pointer<Float>,
);

DynamicLibrary _openLib() {
  return Platform.isAndroid
      ? DynamicLibrary.open('libdj_engine.so')
      : DynamicLibrary.process();
}

/// Decode + BPM/key. No deck involved. Safe to call from a worker isolate.
TrackAnalysis? analyzeTrackFile(String path) {
  final lib = _openLib();
  final fn = lib.lookupFunction<_AnalyzeC, int Function(
    Pointer<Utf8>,
    Pointer<Float>,
    Pointer<Int32>,
    Pointer<Float>,
  )>('dj_analyze_file');
  final p = path.toNativeUtf8();
  final bpm = malloc<Float>();
  final key = malloc<Int32>();
  final offset = malloc<Float>();
  try {
    if (fn(p, bpm, key, offset) != 1) {
      return null;
    }
    return TrackAnalysis(
      bpm: bpm.value,
      key: key.value,
      beatOffset: offset.value,
    );
  } finally {
    malloc.free(p);
    malloc.free(bpm);
    malloc.free(key);
    malloc.free(offset);
  }
}

Future<TrackAnalysis?> analyzeTrackFileOffUi(String path) async {
  try {
    return await Isolate.run(() => analyzeTrackFile(path));
  } catch (_) {
    return analyzeTrackFile(path);
  }
}

double fileDurationSec(String path) {
  final lib = _openLib();
  final fn = lib.lookupFunction<Double Function(Pointer<Utf8>), double Function(Pointer<Utf8>)>(
    'dj_file_duration',
  );
  final p = path.toNativeUtf8();
  try {
    return fn(p);
  } finally {
    malloc.free(p);
  }
}
