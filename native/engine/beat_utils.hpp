#pragma once

// Copyright (C) Mixxx developers
// https://github.com/mixxxdj/mixxx
// Licensed under the GNU General Public License, version 2 or later.
//
// Ported from Mixxx src/track/beatutils.cpp.
// Constant-tempo grid from raw Queen Mary beats. Hop-quantized QM beats
// jitter by ~12 ms; median adjacent gaps lock to 123.05 around 120 BPM.
// Mixxx irons those correction beats, takes the longest stable region, then
// snaps to an integer / 1/12 BPM only if that region's uncertainty window
// contains it.

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace beat_utils {

inline constexpr double kMaxSecsPhaseError = 0.025;
inline constexpr double kMaxSecsPhaseErrorSum = 0.1;
inline constexpr int kMaxOutliersCount = 1;
inline constexpr int kMinRegionBeatCount = 16;

struct ConstRegion {
  double firstBeat = 0.0; // frames
  double beatLength = 0.0;
};

struct ConstTempo {
  double bpm = 0.0;
  double firstBeatFrames = 0.0;
};

inline std::vector<ConstRegion> retrieveConstRegions(const std::vector<double>& coarseBeats,
                                                     double sampleRate) {
  std::vector<ConstRegion> constantRegions;
  if (coarseBeats.size() < 2 || sampleRate <= 0.0) {
    return constantRegions;
  }

  const double maxPhaseError = kMaxSecsPhaseError * sampleRate;
  const double maxPhaseErrorSum = kMaxSecsPhaseErrorSum * sampleRate;
  int leftIndex = 0;
  int rightIndex = (int)coarseBeats.size() - 1;

  while (leftIndex < (int)coarseBeats.size() - 1) {
    if (rightIndex <= leftIndex) {
      break;
    }
    const double meanBeatLength =
        (coarseBeats[(size_t)rightIndex] - coarseBeats[(size_t)leftIndex]) /
        (double)(rightIndex - leftIndex);
    int outliersCount = 0;
    double ironedBeat = coarseBeats[(size_t)leftIndex];
    double phaseErrorSum = 0.0;
    int i = leftIndex + 1;
    for (; i <= rightIndex; ++i) {
      ironedBeat += meanBeatLength;
      const double phaseError = ironedBeat - coarseBeats[(size_t)i];
      phaseErrorSum += phaseError;
      if (std::fabs(phaseError) > maxPhaseError) {
        ++outliersCount;
        if (outliersCount > kMaxOutliersCount || i == leftIndex + 1) {
          break;
        }
      }
      if (std::fabs(phaseErrorSum) > maxPhaseErrorSum) {
        break;
      }
    }
    if (i > rightIndex) {
      double regionBorderError = 0.0;
      if (rightIndex > leftIndex + 2) {
        const double firstBeatLength =
            coarseBeats[(size_t)leftIndex + 1] - coarseBeats[(size_t)leftIndex];
        const double lastBeatLength =
            coarseBeats[(size_t)rightIndex] - coarseBeats[(size_t)rightIndex - 1];
        regionBorderError = std::fabs(firstBeatLength + lastBeatLength - (2.0 * meanBeatLength));
      }
      if (regionBorderError < maxPhaseError / 2.0) {
        constantRegions.push_back({coarseBeats[(size_t)leftIndex], meanBeatLength});
        leftIndex = rightIndex;
        rightIndex = (int)coarseBeats.size() - 1;
        continue;
      }
    }
    --rightIndex;
  }

  constantRegions.push_back({coarseBeats.back(), 0.0});
  return constantRegions;
}

inline std::optional<double> trySnap(double minBpm, double centerBpm, double maxBpm,
                                     double fraction) {
  const double snapBpm = std::round(centerBpm * fraction) / fraction;
  if (snapBpm > minBpm && snapBpm < maxBpm) {
    return snapBpm;
  }
  return std::nullopt;
}

inline double roundBpmWithinRange(double minBpm, double centerBpm, double maxBpm) {
  if (auto snap = trySnap(minBpm, centerBpm, maxBpm, 1.0)) {
    return *snap;
  }
  if (centerBpm < 85.0) {
    if (auto snap = trySnap(minBpm, centerBpm, maxBpm, 2.0)) {
      return *snap;
    }
  }
  if (centerBpm > 127.0) {
    if (auto snap = trySnap(minBpm, centerBpm, maxBpm, 2.0 / 3.0)) {
      return *snap;
    }
  }
  if (auto snap = trySnap(minBpm, centerBpm, maxBpm, 3.0)) {
    return *snap;
  }
  if (auto snap = trySnap(minBpm, centerBpm, maxBpm, 12.0)) {
    return *snap;
  }
  return centerBpm;
}

inline ConstTempo makeConstBpm(const std::vector<ConstRegion>& constantRegions,
                               double sampleRate) {
  ConstTempo out;
  if (constantRegions.size() < 2 || sampleRate <= 0.0) {
    return out;
  }

  int midRegionIndex = 0;
  double longestRegionLength = 0.0;
  double longestRegionBeatLength = 0.0;
  for (int i = 0; i < (int)constantRegions.size() - 1; ++i) {
    const double length =
        constantRegions[(size_t)i + 1].firstBeat - constantRegions[(size_t)i].firstBeat;
    if (length > longestRegionLength) {
      longestRegionLength = length;
      longestRegionBeatLength = constantRegions[(size_t)i].beatLength;
      midRegionIndex = i;
    }
  }
  if (longestRegionLength <= 0.0 || longestRegionBeatLength <= 0.0) {
    return out;
  }

  int longestRegionNumberOfBeats =
      (int)std::lround(longestRegionLength / longestRegionBeatLength);
  if (longestRegionNumberOfBeats < 1) {
    return out;
  }
  double longestRegionBeatLengthMin =
      longestRegionBeatLength - ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
  double longestRegionBeatLengthMax =
      longestRegionBeatLength + ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);

  int startRegionIndex = midRegionIndex;

  for (int i = 0; i < midRegionIndex; ++i) {
    const double length =
        constantRegions[(size_t)i + 1].firstBeat - constantRegions[(size_t)i].firstBeat;
    const int numberOfBeats = (int)std::lround(length / constantRegions[(size_t)i].beatLength);
    if (numberOfBeats < kMinRegionBeatCount) {
      continue;
    }
    const double thisMin = constantRegions[(size_t)i].beatLength -
                           ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
    const double thisMax = constantRegions[(size_t)i].beatLength +
                           ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
    if (longestRegionBeatLength > thisMin && longestRegionBeatLength < thisMax) {
      const double newLongestRegionLength =
          constantRegions[(size_t)midRegionIndex + 1].firstBeat - constantRegions[(size_t)i].firstBeat;
      const double beatLengthMin = std::max(longestRegionBeatLengthMin, thisMin);
      const double beatLengthMax = std::min(longestRegionBeatLengthMax, thisMax);
      const int maxNumberOfBeats = (int)std::lround(newLongestRegionLength / beatLengthMin);
      const int minNumberOfBeats = (int)std::lround(newLongestRegionLength / beatLengthMax);
      if (minNumberOfBeats != maxNumberOfBeats) {
        continue;
      }
      const double newBeatLength = newLongestRegionLength / (double)minNumberOfBeats;
      if (newBeatLength > longestRegionBeatLengthMin && newBeatLength < longestRegionBeatLengthMax) {
        longestRegionLength = newLongestRegionLength;
        longestRegionBeatLength = newBeatLength;
        longestRegionNumberOfBeats = minNumberOfBeats;
        longestRegionBeatLengthMin =
            longestRegionBeatLength - ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
        longestRegionBeatLengthMax =
            longestRegionBeatLength + ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
        startRegionIndex = i;
        break;
      }
    }
  }

  for (int i = (int)constantRegions.size() - 2; i > midRegionIndex; --i) {
    const double length =
        constantRegions[(size_t)i + 1].firstBeat - constantRegions[(size_t)i].firstBeat;
    const int numberOfBeats = (int)std::lround(length / constantRegions[(size_t)i].beatLength);
    if (numberOfBeats < kMinRegionBeatCount) {
      continue;
    }
    const double thisMin = constantRegions[(size_t)i].beatLength -
                           ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
    const double thisMax = constantRegions[(size_t)i].beatLength +
                           ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
    if (longestRegionBeatLength > thisMin && longestRegionBeatLength < thisMax) {
      const double newLongestRegionLength = constantRegions[(size_t)i + 1].firstBeat -
                                            constantRegions[(size_t)startRegionIndex].firstBeat;
      const double minBeatLength = std::max(longestRegionBeatLengthMin, thisMin);
      const double maxBeatLength = std::min(longestRegionBeatLengthMax, thisMax);
      const int maxNumberOfBeats = (int)std::lround(newLongestRegionLength / minBeatLength);
      const int minNumberOfBeats = (int)std::lround(newLongestRegionLength / maxBeatLength);
      if (minNumberOfBeats != maxNumberOfBeats) {
        continue;
      }
      const double newBeatLength = newLongestRegionLength / (double)minNumberOfBeats;
      if (newBeatLength > longestRegionBeatLengthMin && newBeatLength < longestRegionBeatLengthMax) {
        longestRegionLength = newLongestRegionLength;
        longestRegionBeatLength = newBeatLength;
        longestRegionNumberOfBeats = minNumberOfBeats;
        break;
      }
    }
  }

  longestRegionBeatLengthMin =
      longestRegionBeatLength - ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
  longestRegionBeatLengthMax =
      longestRegionBeatLength + ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);

  const double minRoundBpm = 60.0 * sampleRate / longestRegionBeatLengthMax;
  const double maxRoundBpm = 60.0 * sampleRate / longestRegionBeatLengthMin;
  const double centerBpm = 60.0 * sampleRate / longestRegionBeatLength;
  out.bpm = roundBpmWithinRange(minRoundBpm, centerBpm, maxRoundBpm);
  out.firstBeatFrames = constantRegions[(size_t)startRegionIndex].firstBeat;
  return out;
}

inline ConstTempo constTempoFromBeats(const std::vector<double>& beatFrames, double sampleRate) {
  ConstTempo out;
  if (beatFrames.size() < 2) {
    return out;
  }
  if ((int)beatFrames.size() < kMinRegionBeatCount) {
    const double frames = beatFrames.back() - beatFrames.front();
    if (frames > 0.0) {
      out.bpm = 60.0 * (double)(beatFrames.size() - 1) * sampleRate / frames;
      out.firstBeatFrames = beatFrames.front();
    }
    return out;
  }
  const std::vector<ConstRegion> regions = retrieveConstRegions(beatFrames, sampleRate);
  return makeConstBpm(regions, sampleRate);
}

// Walk the grid back to the start of the track, re-snapping to real detected
// beats on the way. Mixxx folds the region start with fmod(), which carries
// the difference between the region period and the rounded period across the
// whole track; over several minutes that reaches half a beat.
// `beats` must be ascending. Returns frames in [0, one beat).
inline double anchorNearStart(double regionFirstBeat, double bpm, double sampleRate,
                              const std::vector<double>& beats) {
  if (bpm < 1.0 || sampleRate <= 0.0 || beats.empty()) {
    return regionFirstBeat;
  }
  const double period = 60.0 * sampleRate / bpm;
  const double tol = kMaxSecsPhaseError * sampleRate;

  double anchor = regionFirstBeat;
  while (anchor - period >= -tol) {
    const double target = anchor - period;
    double snapped = target;
    double bestErr = tol;
    auto it = std::lower_bound(beats.begin(), beats.end(), target);
    if (it != beats.end() && std::fabs(*it - target) < bestErr) {
      bestErr = std::fabs(*it - target);
      snapped = *it;
    }
    if (it != beats.begin()) {
      const double prev = *(it - 1);
      if (std::fabs(prev - target) < bestErr) {
        bestErr = std::fabs(prev - target);
        snapped = prev;
      }
    }
    anchor = snapped;
  }
  while (anchor < 0.0) {
    anchor += period;
  }

  // Average out the ~12 ms hop jitter using the beats just after the anchor.
  double sum = 0.0;
  int count = 0;
  for (double b : beats) {
    const double n = std::round((b - anchor) / period);
    if (n < 0.0 || n > 128.0) {
      continue;
    }
    const double err = b - (anchor + n * period);
    if (std::fabs(err) <= tol) {
      sum += err;
      ++count;
    }
  }
  if (count >= 8) {
    anchor += sum / (double)count;
  }
  while (anchor < 0.0) {
    anchor += period;
  }
  return anchor;
}

} // namespace beat_utils
