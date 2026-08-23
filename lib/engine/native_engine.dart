import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

typedef _CreateC = Pointer<Void> Function();
typedef _DestroyC = Void Function(Pointer<Void>);
typedef _StartC = Int32 Function(Pointer<Void>);
typedef _StopC = Void Function(Pointer<Void>);
typedef _SetModeC = Int32 Function(Pointer<Void>, Int32);
typedef _SetDeviceC = Void Function(Pointer<Void>, Int32);
typedef _GetIntC = Int32 Function(Pointer<Void>);
typedef _LoadC = Int32 Function(Pointer<Void>, Int32, Pointer<Utf8>);
typedef _LoadAnalyzedC = Int32 Function(
  Pointer<Void>,
  Int32,
  Pointer<Utf8>,
  Float,
  Int32,
  Float,
);
typedef _UnloadC = Void Function(Pointer<Void>, Int32);
typedef _PlayC = Void Function(Pointer<Void>, Int32, Int32);
typedef _SeekC = Void Function(Pointer<Void>, Int32, Double);
typedef _PosC = Double Function(Pointer<Void>, Int32);
typedef _PlayingC = Int32 Function(Pointer<Void>, Int32);
typedef _SetFloatDeckC = Void Function(Pointer<Void>, Int32, Float);
typedef _SetEqC = Void Function(Pointer<Void>, Int32, Float, Float, Float);
typedef _SetXfaderC = Void Function(Pointer<Void>, Float);
typedef _SetMasterC = Void Function(Pointer<Void>, Float);
typedef _GetBpmC = Float Function(Pointer<Void>, Int32);
typedef _GetKeyC = Int32 Function(Pointer<Void>, Int32);
typedef _NudgeC = Void Function(Pointer<Void>, Int32, Float);
typedef _SetLoopC = Void Function(Pointer<Void>, Int32, Int32, Float);
typedef _BeatJumpC = Void Function(Pointer<Void>, Int32, Int32);
typedef _SyncC = Void Function(Pointer<Void>, Int32, Int32);
typedef _WaveBinsC = Int32 Function();
typedef _WaveCopyC = Int32 Function(
  Pointer<Void>,
  Int32,
  Pointer<Float>,
  Pointer<Float>,
  Int32,
);

class NativeEngine {
  NativeEngine._(this._lib, this._handle);

  final DynamicLibrary _lib;
  final Pointer<Void> _handle;
  bool started = false;

  late final _destroy = _lib.lookupFunction<_DestroyC, void Function(Pointer<Void>)>(
    'dj_destroy',
  );
  late final _start = _lib.lookupFunction<_StartC, int Function(Pointer<Void>)>(
    'dj_start',
  );
  late final _stop = _lib.lookupFunction<_StopC, void Function(Pointer<Void>)>(
    'dj_stop',
  );
  late final _setMode = _lib
      .lookupFunction<_SetModeC, int Function(Pointer<Void>, int)>(
        'dj_set_output_mode',
      );
  late final _setDevice = _lib
      .lookupFunction<_SetDeviceC, void Function(Pointer<Void>, int)>(
        'dj_set_output_device',
      );
  late final _getMode = _lib
      .lookupFunction<_GetIntC, int Function(Pointer<Void>)>('dj_get_output_mode');
  late final _getChannels = _lib.lookupFunction<_GetIntC, int Function(Pointer<Void>)>(
    'dj_get_output_channels',
  );
  late final _load = _lib
      .lookupFunction<_LoadC, int Function(Pointer<Void>, int, Pointer<Utf8>)>(
        'dj_load',
      );
  late final _loadAnalyzed = _lib.lookupFunction<
    _LoadAnalyzedC,
    int Function(Pointer<Void>, int, Pointer<Utf8>, double, int, double)
  >('dj_load_with_analysis');
  late final _unload = _lib
      .lookupFunction<_UnloadC, void Function(Pointer<Void>, int)>('dj_unload');
  late final _play = _lib
      .lookupFunction<_PlayC, void Function(Pointer<Void>, int, int)>('dj_play');
  late final _seek = _lib
      .lookupFunction<_SeekC, void Function(Pointer<Void>, int, double)>('dj_seek');
  late final _position = _lib
      .lookupFunction<_PosC, double Function(Pointer<Void>, int)>('dj_position');
  late final _duration = _lib
      .lookupFunction<_PosC, double Function(Pointer<Void>, int)>('dj_duration');
  late final _playing = _lib
      .lookupFunction<_PlayingC, int Function(Pointer<Void>, int)>('dj_playing');
  late final _setGain = _lib
      .lookupFunction<_SetFloatDeckC, void Function(Pointer<Void>, int, double)>(
        'dj_set_gain',
      );
  late final _setEq = _lib
      .lookupFunction<
        _SetEqC,
        void Function(Pointer<Void>, int, double, double, double)
      >('dj_set_eq');
  late final _setFilter = _lib
      .lookupFunction<_SetFloatDeckC, void Function(Pointer<Void>, int, double)>(
        'dj_set_filter',
      );
  late final _setFader = _lib
      .lookupFunction<_SetFloatDeckC, void Function(Pointer<Void>, int, double)>(
        'dj_set_fader',
      );
  late final _setXfader = _lib
      .lookupFunction<_SetXfaderC, void Function(Pointer<Void>, double)>(
        'dj_set_xfader',
      );
  late final _setMaster = _lib
      .lookupFunction<_SetMasterC, void Function(Pointer<Void>, double)>(
        'dj_set_master',
      );
  late final _setRate = _lib
      .lookupFunction<_SetFloatDeckC, void Function(Pointer<Void>, int, double)>(
        'dj_set_rate',
      );
  late final _getBpm = _lib
      .lookupFunction<_GetBpmC, double Function(Pointer<Void>, int)>('dj_get_bpm');
  late final _getKey = _lib
      .lookupFunction<_GetKeyC, int Function(Pointer<Void>, int)>('dj_get_key');
  late final _getBeatOffset = _lib
      .lookupFunction<_GetBpmC, double Function(Pointer<Void>, int)>(
        'dj_get_beat_offset',
      );
  late final _nudgeGrid = _lib
      .lookupFunction<_NudgeC, void Function(Pointer<Void>, int, double)>(
        'dj_nudge_grid',
      );
  late final _setLoop = _lib
      .lookupFunction<_SetLoopC, void Function(Pointer<Void>, int, int, double)>(
        'dj_set_loop',
      );
  late final _loopStart = _lib
      .lookupFunction<_PosC, double Function(Pointer<Void>, int)>('dj_loop_start');
  late final _loopEnd = _lib
      .lookupFunction<_PosC, double Function(Pointer<Void>, int)>('dj_loop_end');
  late final _beatJump = _lib
      .lookupFunction<_BeatJumpC, void Function(Pointer<Void>, int, int)>(
        'dj_beat_jump',
      );
  late final _syncTo = _lib
      .lookupFunction<_SyncC, void Function(Pointer<Void>, int, int)>('dj_sync_to');
  late final _waveBins = _lib.lookupFunction<_WaveBinsC, int Function()>(
    'dj_waveform_bins',
  );
  late final _waveCopy = _lib
      .lookupFunction<
        _WaveCopyC,
        int Function(
          Pointer<Void>,
          int,
          Pointer<Float>,
          Pointer<Float>,
          int,
        )
      >('dj_waveform_copy');

  static NativeEngine open() {
    final lib = Platform.isAndroid
        ? DynamicLibrary.open('libdj_engine.so')
        : DynamicLibrary.process();
    final create = lib.lookupFunction<_CreateC, _CreateC>('dj_create');
    final handle = create();
    final engine = NativeEngine._(lib, handle);
    engine.started = engine._start(handle) == 1;
    return engine;
  }

  bool start() {
    started = _start(_handle) == 1;
    return started;
  }

  void dispose() {
    _stop(_handle);
    _destroy(_handle);
  }

  bool setExternalMixer(bool enabled, {int deviceId = 0}) {
    _setDevice(_handle, deviceId);
    final ok = _setMode(_handle, enabled ? 1 : 0) == 1;
    if (ok) started = true;
    return ok;
  }

  int get outputMode => _getMode(_handle);
  int get outputChannels => _getChannels(_handle);
  int get nativeHandle => _handle.address;

  bool load(
    int deck,
    String path, {
    double? bpm,
    int? key,
    double? beatOffset,
  }) {
    final p = path.toNativeUtf8();
    try {
      if (bpm != null && bpm > 1) {
        return _loadAnalyzed(
              _handle,
              deck,
              p,
              bpm,
              key ?? -1,
              beatOffset ?? 0,
            ) ==
            1;
      }
      return _load(_handle, deck, p) == 1;
    } finally {
      malloc.free(p);
    }
  }

  void unload(int deck) => _unload(_handle, deck);
  void play(int deck, bool playing) => _play(_handle, deck, playing ? 1 : 0);
  void seek(int deck, double seconds) => _seek(_handle, deck, seconds);
  double position(int deck) => _position(_handle, deck);
  double duration(int deck) => _duration(_handle, deck);
  bool isPlaying(int deck) => _playing(_handle, deck) == 1;
  void setGain(int deck, double v) => _setGain(_handle, deck, v);
  void setEq(int deck, double low, double mid, double high) =>
      _setEq(_handle, deck, low, mid, high);
  void setFilter(int deck, double amount) => _setFilter(_handle, deck, amount);
  void setFader(int deck, double v) => _setFader(_handle, deck, v);
  void setXfader(double x) => _setXfader(_handle, x);
  void setMaster(double v) => _setMaster(_handle, v);
  void setRate(int deck, double rate) => _setRate(_handle, deck, rate);
  double bpm(int deck) => _getBpm(_handle, deck);
  int key(int deck) => _getKey(_handle, deck);
  double beatOffset(int deck) => _getBeatOffset(_handle, deck);
  void nudgeGrid(int deck, double seconds) => _nudgeGrid(_handle, deck, seconds);
  void setLoop(int deck, bool enabled, double bars) =>
      _setLoop(_handle, deck, enabled ? 1 : 0, bars);
  double loopStart(int deck) => _loopStart(_handle, deck);
  double loopEnd(int deck) => _loopEnd(_handle, deck);
  void beatJump(int deck, int beats) => _beatJump(_handle, deck, beats);
  void syncTo(int slave, int master) => _syncTo(_handle, slave, master);

  ({Float32List min, Float32List max}) waveform(int deck) {
    final bins = _waveBins();
    final minPtr = malloc<Float>(bins);
    final maxPtr = malloc<Float>(bins);
    try {
      final n = _waveCopy(_handle, deck, minPtr, maxPtr, bins);
      return (
        min: Float32List.fromList(minPtr.asTypedList(n)),
        max: Float32List.fromList(maxPtr.asTypedList(n)),
      );
    } finally {
      malloc.free(minPtr);
      malloc.free(maxPtr);
    }
  }
}
