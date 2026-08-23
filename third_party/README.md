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

## dr_libs (dr_mp3 / dr_wav) — Public domain / MIT-0
- Path: `third_party/dr_libs`
- Used for: MP3/WAV decode

## qm-dsp (beat + key subset) — GPL-2.0-or-later
- Path: `third_party/qm-dsp`
- Upstream: https://github.com/c4dm/qm-dsp
- Used for: Queen Mary TempoTrackV2 raw beats; GetKeyMode (chromagram / ConstantQ)
- Constant-tempo BPM: Mixxx BeatUtils port in `native/engine/beat_utils.hpp`
- kissfft inside `ext/kissfft` is BSD-3-Clause
