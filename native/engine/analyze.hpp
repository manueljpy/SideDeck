#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "dsp/keydetection/GetKeyMode.h"

// Queen Mary TempoTrackV2 + Mixxx BeatUtils for tempo and grid; GetKeyMode for key.
struct AnalysisResult {
  float bpm = 120.0f;
  float beatOffsetSec = 0.0f;
  int key = -1; // 0..11 major, 12..23 minor; -1 unknown
  std::vector<float> beats; // seconds
};

namespace analyze_detail {

// First 60s of the file, mixed to mono. Playback still uses the full
// file; BPM/key/grid only need a short prefix.
inline constexpr float kAnalyzeSr = 22050.0f;
inline constexpr float kAnalyzeWindowSec = 60.0f;

// GetKeyMode chroma max is MIDI 96 (~2.1 kHz). Keep Nyquist above that
// after the built-in downsample (default 8× at 44.1 kHz).
inline int keyDecimationFactor(float sampleRate) {
  int decim = 8;
  while (decim > 2 && sampleRate / (float)decim < 4186.0f) {
    decim /= 2;
  }
  return decim;
}

inline int estimateKey(const float* interleaved, int frames, int channels, float sampleRate) {
  if (!interleaved || frames < 1 || channels < 1 || sampleRate < 1.0f) {
    return -1;
  }
  try {
    GetKeyMode::Config cfg((double)sampleRate, 440.0f);
    cfg.decimationFactor = keyDecimationFactor(sampleRate);
    GetKeyMode km(cfg);
    const int block = km.getBlockSize();
    const int hop = km.getHopSize();
    if (block < 1 || hop < 1 || frames < block) {
      return -1;
    }
    std::vector<double> frame((size_t)block);
    int counts[25] = {};
    int n = 0;
    for (int pos = 0; pos + block <= frames; pos += hop) {
      for (int i = 0; i < block; ++i) {
        const float* s = interleaved + (size_t)(pos + i) * (size_t)channels;
        frame[(size_t)i] =
            channels <= 1 ? (double)s[0] : 0.5 * ((double)s[0] + (double)s[1]);
      }
      const int k = km.process(frame.data());
      if (k >= 1 && k <= 24) {
        counts[k]++;
        ++n;
      }
    }
    if (n == 0) {
      return -1;
    }
    int best = 1;
    for (int k = 2; k <= 24; ++k) {
      if (counts[k] > counts[best]) {
        best = k;
      }
    }
    // QM: 1=Cmaj .. 12=Bmaj, 13=Cmin .. 24=Bmin. Ours: 0..11 maj, 12..23 min.
    return best <= 12 ? best - 1 : (best - 13) + 12;
  } catch (...) {
    return -1;
  }
}

inline float snapDanceBpm(float bpm) {
  if (bpm < 1.0f) {
    return 120.0f;
  }
  while (bpm < 85.0f) {
    bpm *= 2.0f;
  }
  while (bpm > 175.0f) {
    bpm *= 0.5f;
  }
  return bpm;
}

} // namespace analyze_detail

#include "analyze_qm.hpp"

inline void applyKickGrid(AnalysisResult& out, const float* interleaved, int frames, int channels,
                          float sampleRate) {
  const double periodSec = 60.0 / (double)out.bpm;
  const double duration = (double)frames / (double)sampleRate;
  const grid_phase::KickEnvelope env =
      grid_phase::buildKickEnvelope(interleaved, frames, channels, (double)sampleRate);
  double phaseSec = grid_phase::bestPhase(env, periodSec, duration);
  if (phaseSec < 0.0) {
    phaseSec = 0.0;
  }
  out.beatOffsetSec = (float)grid_phase::firstAudibleLine(
      interleaved, frames, channels, (double)sampleRate, phaseSec, periodSec);
  out.beats.clear();
  for (int i = 0;; ++i) {
    const float t = (float)(phaseSec + (double)i * periodSec);
    if (t >= (float)duration) {
      break;
    }
    if (t >= 0.0f) {
      out.beats.push_back(t);
    }
  }
}

// knownBpm > 1 skips Queen Mary (tag TBPM). knownKey in 0..23 skips chroma key
// (tag TKEY). Grid phase still runs so the beatgrid is not stuck at 0.
inline AnalysisResult analyzeTrack(const float* interleaved, int frames, int channels,
                                   float sampleRate, float knownBpm = 0.0f, int knownKey = -1) {
  AnalysisResult out;
  if (knownBpm > 1.0f) {
    out.bpm = analyze_detail::snapDanceBpm(knownBpm);
    applyKickGrid(out, interleaved, frames, channels, sampleRate);
  } else {
    out = analyzeTrackQueenMary(interleaved, frames, channels, sampleRate);
  }
  if (knownKey >= 0 && knownKey <= 23) {
    out.key = knownKey;
  } else if (out.bpm > 1.0f && !out.beats.empty()) {
    out.key = analyze_detail::estimateKey(interleaved, frames, channels, sampleRate);
  }
  return out;
}
