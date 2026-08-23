#pragma once

#include "beat_utils.hpp"
#include "grid_phase.hpp"
#include "dsp/onsets/DetectionFunction.h"
#include "dsp/tempotracking/TempoTrackV2.h"
#include "maths/MathUtilities.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Queen Mary TempoTrackV2 + Mixxx BeatUtils for BPM. Grid phase comes from
// kick energy (grid_phase.hpp): QM often locks onto the offbeat hat.
inline AnalysisResult analyzeTrackQueenMary(const float* interleaved, int frames,
                                            int channels, float sampleRate) {
  AnalysisResult out;
  if (frames < (int)sampleRate || channels < 1 || !interleaved) {
    return out;
  }
  try {
    constexpr float kStepSecs = 0.01161f;
    constexpr int kMaximumBinSizeHz = 50;
    const int stepSize = std::max(1, (int)(sampleRate * kStepSecs));
    const int windowSize = MathUtilities::nextPowerOfTwo((int)(sampleRate / kMaximumBinSizeHz));

    DFConfig cfg{};
    cfg.stepSize = stepSize;
    cfg.frameLength = windowSize;
    cfg.DFType = DF_COMPLEXSD;
    cfg.dbRise = 3;
    cfg.adaptiveWhitening = false;
    cfg.whiteningRelaxCoeff = -1;
    cfg.whiteningFloor = -1;

    DetectionFunction df(cfg);
    std::vector<double> frame((size_t)windowSize, 0.0);
    std::vector<double> detection;
    detection.reserve((size_t)frames / (size_t)stepSize + 8);

    const int ch = channels;
    for (int pos = 0; pos + windowSize <= frames; pos += stepSize) {
      for (int i = 0; i < windowSize; ++i) {
        const float* s = interleaved + (size_t)(pos + i) * (size_t)ch;
        if (ch == 1) {
          frame[(size_t)i] = (double)s[0];
        } else {
          frame[(size_t)i] = 0.5 * ((double)s[0] + (double)s[1]);
        }
      }
      detection.push_back(df.processTimeDomain(frame.data()));
    }

    size_t nonZero = detection.size();
    while (nonZero > 0 && detection[nonZero - 1] <= 0.0) {
      --nonZero;
    }
    std::vector<double> dfv;
    if (nonZero > 2) {
      dfv.assign(detection.begin() + 2, detection.begin() + (std::ptrdiff_t)nonZero);
    }
    if (dfv.size() < 8) {
      return out;
    }

    std::vector<double> beatPeriod(dfv.size(), 0.0);
    std::vector<double> tempi;
    std::vector<double> beats;
    TempoTrackV2 tt(sampleRate, stepSize);
    tt.calculateBeatPeriod(dfv, beatPeriod, tempi);
    tt.calculateBeats(dfv, beatPeriod, beats);
    if (beats.size() < 4) {
      return out;
    }

    std::vector<double> beatFrames;
    beatFrames.reserve(beats.size());
    const double durationFrames = (double)frames;
    for (double b : beats) {
      const double f = b * (double)stepSize + (double)(stepSize / 2);
      if (f >= 0.0 && f < durationFrames) {
        beatFrames.push_back(f);
      }
    }
    if (beatFrames.size() < 4) {
      return out;
    }

    beat_utils::ConstTempo tempo = beat_utils::constTempoFromBeats(beatFrames, (double)sampleRate);
    if (tempo.bpm < 1.0) {
      return out;
    }
    out.bpm = analyze_detail::snapDanceBpm((float)tempo.bpm);

    const double periodSec = 60.0 / (double)out.bpm;
    const double duration = (double)frames / (double)sampleRate;

    // Queen Mary's phase follows the loudest onset, which is the offbeat hat
    // on a lot of house. Take the period from it and let the kick energy pick
    // the phase; fall back to the detected beats if the track has no low end.
    const grid_phase::KickEnvelope env =
        grid_phase::buildKickEnvelope(interleaved, frames, ch, (double)sampleRate);
    double phaseSec = grid_phase::bestPhase(env, periodSec, duration);
    if (phaseSec < 0.0) {
      const double phaseFrames = beat_utils::anchorNearStart(
          tempo.firstBeatFrames, (double)out.bpm, (double)sampleRate, beatFrames);
      phaseSec = std::fmod(phaseFrames / (double)sampleRate, periodSec);
    }
    if (phaseSec < 0.0) {
      phaseSec += periodSec;
    }

    out.beatOffsetSec = (float)grid_phase::firstAudibleLine(interleaved, frames, ch,
                                                            (double)sampleRate, phaseSec, periodSec);

    for (int i = 0;; ++i) {
      const float t = (float)(phaseSec + (double)i * periodSec);
      if (t >= (float)duration) {
        break;
      }
      if (t >= 0.0f) {
        out.beats.push_back(t);
      }
    }
    return out;
  } catch (...) {
    return AnalysisResult{};
  }
}
