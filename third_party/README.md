# Vendored dependencies

## Signalsmith Stretch — MIT
- Path: `third_party/signalsmith-stretch`
- Upstream: https://github.com/Signalsmith-Audio/signalsmith-stretch
- Used for: keylock time-stretch (pitch held while tempo changes)

## Signalsmith Linear — MIT
- Path: `third_party/signalsmith-linear`
- Upstream: https://github.com/Signalsmith-Audio/linear
- Used for: FFT / STFT inside Signalsmith Stretch

## Oboe — Apache-2.0
- Path: `third_party/oboe`
- Used for: Android low-latency audio I/O

## dr_libs (dr_mp3 / dr_wav / dr_flac) — Public domain / MIT-0
- Path: `third_party/dr_libs`
- Used for: MP3 / WAV / FLAC decode

## libogg — BSD-style
- Path: `third_party/ogg`
- Upstream: https://github.com/xiph/ogg (v1.3.5)
- Used for: Ogg container demux (Opus)
- Vendored subset: sources + CMake + COPYING (no CI/docs/examples).
  `ogg.pc.in` kept because upstream CMake still configures it.

## libopus — BSD-style
- Path: `third_party/opus`
- Upstream: https://github.com/xiph/opus (v1.5.2; DNN weights omitted)
- Used for: Opus decode
- Vendored subset: celt/silk/src + CMake + COPYING (no tests/meson/CI).
  `Makefile.am` and `lpcnet_*.mk` kept because upstream CMake parses them
  even when DNN/tests are disabled.

## opusfile — BSD-style
- Path: `third_party/opusfile`
- Upstream: https://github.com/xiph/opusfile (v0.12)
- Used for: seekable Opus-in-Ogg decode (`.opus` / `.ogg`)
- Built without HTTP/OpenSSL (`http.c` not compiled)
- Vendored subset: decode sources + COPYING (no examples/CI)

## qm-dsp (beat + key subset) — GPL-2.0-or-later
- Path: `third_party/qm-dsp`
- Upstream: https://github.com/c4dm/qm-dsp
- Used for: Queen Mary TempoTrackV2 raw beats; GetKeyMode (chromagram / ConstantQ)
- Constant-tempo BPM: Mixxx BeatUtils port in `native/engine/beat_utils.hpp`
- kissfft inside `ext/kissfft` is BSD-3-Clause
