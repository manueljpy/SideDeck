#include "dj_engine.h"

#include "analyze.hpp"
#include "biquad.hpp"
#include "id3_meta.hpp"

#include "signalsmith-stretch.h"
#include <oboe/Oboe.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <jni.h>
#endif

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "chunk_cache.hpp"

#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "sidedeck", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "sidedeck", __VA_ARGS__)

namespace {

// ~10–20 ms per bin on typical tracks so the zoomed view has real transients.
constexpr int kWaveformBins = 32768;
constexpr int kHotCues = 4;
constexpr int kMaxCallbackFrames = 2048;
constexpr int kMaxStretchInFrames = kMaxCallbackFrames * 2 + 256;
// Loop wrap crossfade, in source frames (~10 ms at 48 kHz). Long enough to
// hide a bass phase jump at the splice.
constexpr int kLoopFadeFrames = 512;

constexpr float kEqLowHz = 246.0f;
constexpr float kEqHighHz = 2484.0f;
constexpr float kFilterMinHz = 13.0f;
constexpr float kFilterMaxHz = 22050.0f;
constexpr float kXfaderTransform = 1.0f;

inline float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

// Isolator fader: 0 = kill, 1 = unity, 4 = +12 dB.
inline float eqBandGain(float db) {
  if (db >= 0.0f) {
    return std::pow(10.0f, db / 20.0f);
  }
  return std::max(0.0f, 1.0f + db / 12.0f);
}

// Club mixer xfader. x: 0 = A, 1 = B. Assigned deck stays at unity through
// center; the other side is 1 - |2x-1|^transform.
inline void xfaderGains(float x, float& gainA, float& gainB) {
  const float pos = 2.0f * clamp01(x) - 1.0f;
  if (pos < 0.0f) {
    gainA = 1.0f;
    gainB = 1.0f - std::pow(-pos, kXfaderTransform);
  } else if (pos > 0.0f) {
    gainA = 1.0f - std::pow(pos, kXfaderTransform);
    gainB = 1.0f;
  } else {
    gainA = 1.0f;
    gainB = 1.0f;
  }
}

struct Deck {
  std::mutex mutex;
  ChunkCache cache;
  std::string path;
  int64_t totalFrames = 0;
  int sampleRate = 48000;
  int channels = 2;
  double playhead = 0.0; // audible file position in frames (fractional)
  bool playing = false;
  bool loaded = false;
  float gain = 1.0f;
  float fader = 1.0f;
  float rate = 1.0f;
  bool keylock = true;
  float filter = 0.0f; // -1 HPF .. 0 flat .. +1 LPF
  float eqLow = 0, eqMid = 0, eqHigh = 0;
  float eqLowG = 1, eqMidG = 1, eqHighG = 1;
  double cuePoint = 0.0;
  double hotcues[kHotCues] = {-1, -1, -1, -1};
  bool loopEnabled = false;
  float loopBars = 1.0f;
  double loopStart = 0.0;
  double loopEnd = 0.0;

  float bpm = 120.0f;
  int key = -1;
  float beatOffset = 0.0f;
  std::atomic<int64_t> pendingJump{-1};
  std::atomic<bool> pendingLoopShift{false};
  std::atomic<double> pendingLoopDelta{0.0};
  std::vector<float> jumpXfL, jumpXfR;
  std::vector<float> loopXfL, loopXfR;
  float gate = 0.0f;
  bool pauseXf = false;

  std::vector<float> waveMin;
  std::vector<float> waveMax;

  LR4 eqLoL, eqLoR;         // LPF @ 246 Hz
  LR4 eqMidHpL, eqMidHpR;   // HPF @ 246 Hz
  LR4 eqMidLpL, eqMidLpR;   // LPF @ 2484 Hz
  LR4 eqHiL, eqHiR;         // HPF @ 2484 Hz
  Biquad filtL, filtR;

  // Signalsmith Stretch: tempo change with pitch held (keylock).
  // playhead is the audible position; stretchRead is the input cursor (ahead by latency).
  signalsmith::stretch::SignalsmithStretch<float> stretcher;
  bool stretcherReady = false;
  bool stretchPrimed = false;
  int stretcherSr = 0;
  double stretchInAccum = 0.0;
  double stretchRead = 0.0;
  std::vector<float> stretchInL, stretchInR, stretchOutL, stretchOutR;

  // Wrap at loopEnd without a click: over the last kLoopFadeFrames of the
  // loop, crossfade into the run-up before loopStart. The fade lands exactly
  // on the splice, so loopEnd meets loopStart on a continuous waveform and the
  // downbeat at loopStart itself is never smeared. Length is fixed and driven
  // by absolute position, so it does not depend on where loopEnd happens to
  // fall inside a callback buffer.
  double readLooped(double pos, double step, float* L, float* R, int n, bool allowWrap,
                    bool priming = false) {
    const double sr = (double)std::max(1, sampleRate);
    const double st = step < 1.0e-9 ? 1.0 : step;
    const double a = loopStart * sr;
    const double b = loopEnd * sr;
    const double len = b - a;
    if (!allowWrap || !loopEnabled || len <= 1.0e-6) {
      cache.readRun(pos, st, L, R, n, priming);
      return pos + (double)n * st;
    }
    // Stay inside the loop, and only fade from run-up that exists. A loop
    // pinned to the start of the file gets a hard splice.
    double fade = std::min((double)kLoopFadeFrames, std::min(len * 0.5, a));
    if (fade < 1.0) {
      fade = 0.0;
    }
    const double fadeStart = b - fade;
    int done = 0;
    while (done < n) {
      if (pos >= b) {
        double rel = std::fmod(pos - a, len);
        if (rel < 0.0) {
          rel += len;
        }
        pos = a + rel;
      }
      const bool fading = fade > 0.0 && pos >= fadeStart;
      int cnt = (int)std::ceil(((fading ? b : fadeStart) - pos) / st);
      cnt = std::max(1, std::min(cnt, n - done));
      cache.readRun(pos, st, L + done, R + done, cnt, priming);
      if (fading) {
        cache.readRun(pos - len, st, loopXfL.data(), loopXfR.data(), cnt, priming);
        for (int k = 0; k < cnt; ++k) {
          const float u = clamp01((float)((pos + (double)k * st - fadeStart) / fade));
          L[done + k] = L[done + k] * (1.0f - u) + loopXfL[k] * u;
          R[done + k] = R[done + k] * (1.0f - u) + loopXfR[k] * u;
        }
      }
      pos += (double)cnt * st;
      done += cnt;
    }
    return pos;
  }

  // Every position you can jump to, re-hinted on each buffer so its chunk
  // stays resident in the cache. That is what makes a cue jump land on
  // decoded audio instead of waiting for the worker.
  void hintAnchors() {
    const int sr = std::max(1, sampleRate);
    ChunkCache::Span spans[3 + kHotCues];
    int n = 0;
    spans[n++] = {(int64_t)(cuePoint * (double)sr), ChunkCache::kChunkFrames};
    for (const double hotcue : hotcues) {
      if (hotcue >= 0.0) {
        spans[n++] = {(int64_t)(hotcue * (double)sr), ChunkCache::kChunkFrames};
      }
    }
    if (loopEnabled) {
      // A chunk before loopStart as well: the wrap crossfade reads the run-up.
      const int64_t loopStartFrame = (int64_t)(loopStart * (double)sr);
      spans[n++] = {loopStartFrame - ChunkCache::kChunkFrames,
                    ChunkCache::kChunkFrames * 3};
      const int64_t loopEndFrame = (int64_t)(loopEnd * (double)sr);
      spans[n++] = {loopEndFrame - ChunkCache::kChunkFrames,
                    ChunkCache::kChunkFrames * 2};
    }
    cache.hintEngineSpans(spans, n);
  }

  void hintHot() {
    const int sr = std::max(1, sampleRate);
    cache.setHotFrame((int64_t)playhead);
    cache.hintEngineFrames((int64_t)playhead, sr);
    hintAnchors();
  }

  void commitJumpTo(double destFrames, bool shiftLoop = false, double loopDelta = 0.0) {
    cache.jumpTo((int64_t)destFrames);
    hintAnchors();
    if (shiftLoop) {
      pendingLoopDelta.store(loopDelta);
      pendingLoopShift.store(true);
    } else {
      pendingLoopShift.store(false);
    }
    if (playing) {
      pendingJump.store((int64_t)destFrames);
    } else {
      applyJump(destFrames);
    }
  }

  void ensureStretcher() {
    if (stretcherReady && stretcherSr == sampleRate) {
      return;
    }
    stretcher.presetCheaper(2, (float)sampleRate, true);
    stretcher.setTransposeFactor(1.0f);
    stretcherSr = sampleRate;
    stretcherReady = true;
    stretchInAccum = 0.0;
    stretchPrimed = false;
    growReadBuffers(stretcher.outputSeekLength(2.0f) + kMaxCallbackFrames * 2 + 256);
  }

  // readLooped crossfades into loopXf, so it must hold as many frames as the
  // stretcher can ever ask for in one go.
  void growReadBuffers(int frames) {
    if ((int)stretchInL.size() >= frames) {
      return;
    }
    stretchInL.resize((size_t)frames, 0.0f);
    stretchInR.resize((size_t)frames, 0.0f);
    loopXfL.resize((size_t)frames, 0.0f);
    loopXfR.resize((size_t)frames, 0.0f);
  }

  void flushStretcher() {
    if (stretcherReady) {
      stretcher.reset();
    }
    stretchInAccum = 0.0;
    stretchPrimed = false;
  }

  // After reset, Stretch's output lags the input cursor. Prime so the next
  // process() block is aligned to playhead (the audible / waveform position).
  void primeStretcher(float rate) {
    ensureStretcher();
    int need = stretcher.outputSeekLength(rate);
    if (need < 1) {
      need = 1;
    }
    growReadBuffers(need);
    stretchRead =
        readLooped(playhead, 1.0, stretchInL.data(), stretchInR.data(), need, loopEnabled, true);
    float* ptrs[2] = {stretchInL.data(), stretchInR.data()};
    stretcher.outputSeek(ptrs, need);
    stretchPrimed = true;
  }

  void resetFilters() {
    eqLoL.reset();
    eqLoR.reset();
    eqMidHpL.reset();
    eqMidHpR.reset();
    eqMidLpL.reset();
    eqMidLpR.reset();
    eqHiL.reset();
    eqHiR.reset();
    filtL.reset();
    filtR.reset();
  }

  void applyJump(double newPlayhead) {
    playhead = newPlayhead;
    if (pendingLoopShift.exchange(false)) {
      shiftLoop(pendingLoopDelta.load());
    }
    flushStretcher();
    resetFilters();
  }

  void rebuildEq(float sr) {
    eqLoL.lowPass(sr, kEqLowHz);
    eqLoR.lowPass(sr, kEqLowHz);
    eqMidHpL.highPass(sr, kEqLowHz);
    eqMidHpR.highPass(sr, kEqLowHz);
    eqMidLpL.lowPass(sr, kEqHighHz);
    eqMidLpR.lowPass(sr, kEqHighHz);
    eqHiL.highPass(sr, kEqHighHz);
    eqHiR.highPass(sr, kEqHighHz);
    eqLowG = eqBandGain(eqLow);
    eqMidG = eqBandGain(eqMid);
    eqHighG = eqBandGain(eqHigh);
  }

  void rebuildFilter(float sr) {
    const float mag = std::fabs(filter);
    if (mag <= 0.02f) {
      filtL.identity();
      filtR.identity();
      return;
    }
    const float t = (mag - 0.02f) / 0.98f;
    const float fMax = std::min(kFilterMaxHz, std::max(100.0f, sr * 0.45f));
    if (filter > 0.0f) {
      const float freq = fMax * std::pow(kFilterMinHz / fMax, t);
      filtL.lowPass(sr, freq, LR4::kButterQ);
      filtR.lowPass(sr, freq, LR4::kButterQ);
    } else {
      const float freq = kFilterMinHz * std::pow(fMax / kFilterMinHz, t);
      filtL.highPass(sr, freq, LR4::kButterQ);
      filtR.highPass(sr, freq, LR4::kButterQ);
    }
  }

  double loopLengthSec() const {
    const double beat = 60.0 / (double)std::max(1.0f, bpm);
    return beat * 4.0 * (double)loopBars;
  }

  void captureLoopAtPlayhead() {
    loopStart = playhead / (double)std::max(1, sampleRate);
    loopEnd = loopStart + loopLengthSec();
  }

  void resizeLoopFromStart() {
    loopEnd = loopStart + loopLengthSec();
  }

  void wrapPlayheadIntoLoopIfNeeded() {
    if (!loopEnabled || sampleRate < 1) {
      return;
    }
    const double sr = (double)sampleRate;
    const double len = loopEnd - loopStart;
    if (len <= 1.0e-6) {
      return;
    }
    const double pos = playhead / sr;
    if (pos >= loopStart && pos < loopEnd) {
      return;
    }
    double rel = std::fmod(pos - loopStart, len);
    if (rel < 0.0) {
      rel += len;
    }
    commitJumpTo((loopStart + rel) * sr);
  }

  void shiftLoop(double deltaSec) {
    const double len = loopEnd - loopStart;
    loopStart = std::max(0.0, loopStart + deltaSec);
    loopEnd = loopStart + len;
  }
};

std::vector<float> resampleMono(const std::vector<float>& mono, int srcSr, int dstSr) {
  if (srcSr <= 0 || dstSr <= 0 || mono.empty() || srcSr == dstSr) {
    return mono;
  }
  const int frames = (int)mono.size();
  const double ratio = (double)dstSr / (double)srcSr;
  const int outFrames = std::max(1, (int)(frames * ratio));
  std::vector<float> out((size_t)outFrames);
  for (int i = 0; i < outFrames; ++i) {
    const double src = (double)i / ratio;
    const int i0 = std::min((int)src, frames - 1);
    const int i1 = std::min(i0 + 1, frames - 1);
    const float frac = (float)(src - (double)i0);
    out[(size_t)i] = mono[(size_t)i0] * (1.0f - frac) + mono[(size_t)i1] * frac;
  }
  return out;
}

std::vector<float> interleavedToMono(const std::vector<float>& interleaved, unsigned int channels,
                                     drmp3_uint64 frames) {
  if (channels <= 1) {
    if (interleaved.size() > (size_t)frames) {
      return std::vector<float>(interleaved.begin(), interleaved.begin() + (size_t)frames);
    }
    return interleaved;
  }
  std::vector<float> mono((size_t)frames);
  for (drmp3_uint64 i = 0; i < frames; ++i) {
    const size_t base = (size_t)i * (size_t)channels;
    mono[(size_t)i] = 0.5f * (interleaved[base] + interleaved[base + 1]);
  }
  return mono;
}

struct AtomicFlag {
  std::atomic<bool>& flag;
  explicit AtomicFlag(std::atomic<bool>& f) : flag(f) { flag.store(true); }
  ~AtomicFlag() { flag.store(false); }
};

struct Engine : public oboe::AudioStreamDataCallback,
                public oboe::AudioStreamErrorCallback {
  std::mutex streamMutex;
  std::shared_ptr<oboe::AudioStream> stream;
  Deck decks[2];
  std::atomic<float> xfader{0.5f};
  std::atomic<float> master{1.0f};
  std::atomic<int> outputMode{0}; // 0 internal 2ch, 1 external 4ch
  std::atomic<int> engineSampleRate{48000};
  std::atomic<int> engineChannels{2};
  std::atomic<bool> alive{true};
  std::atomic<bool> reconnecting{false};
  std::atomic<bool> managingStream{false};
  std::vector<float> mixAL, mixAR, mixBL, mixBR;

  Engine() {
    mixAL.assign(kMaxCallbackFrames, 0.0f);
    mixAR.assign(kMaxCallbackFrames, 0.0f);
    mixBL.assign(kMaxCallbackFrames, 0.0f);
    mixBR.assign(kMaxCallbackFrames, 0.0f);
    for (auto& d : decks) {
      d.stretchInL.assign((size_t)kMaxStretchInFrames, 0.0f);
      d.stretchInR.assign((size_t)kMaxStretchInFrames, 0.0f);
      d.stretchOutL.assign((size_t)kMaxCallbackFrames, 0.0f);
      d.stretchOutR.assign((size_t)kMaxCallbackFrames, 0.0f);
      d.jumpXfL.assign((size_t)kMaxCallbackFrames, 0.0f);
      d.jumpXfR.assign((size_t)kMaxCallbackFrames, 0.0f);
      d.loopXfL.assign((size_t)kMaxStretchInFrames, 0.0f);
      d.loopXfR.assign((size_t)kMaxStretchInFrames, 0.0f);
    }
  }

  ~Engine() {
    alive.store(false);
    closeStream();
    decks[0].cache.stop();
    decks[1].cache.stop();
  }

  void applyLoadedDeckSampleRate(int newSr) {
    for (auto& d : decks) {
      std::string path;
      double playhead = 0;
      bool loaded = false;
      bool wasPlaying = false;
      int oldSr = 0;
      {
        std::lock_guard<std::mutex> dl(d.mutex);
        loaded = d.loaded;
        path = d.path;
        playhead = d.playhead;
        oldSr = d.sampleRate;
        wasPlaying = d.playing;
        if (loaded && !path.empty() && oldSr > 0 && oldSr != newSr) {
          // Reopening the cache tears down its slots. Park the deck first so
          // a render in flight reads silence instead of a half-open cache.
          d.loaded = false;
          d.playing = false;
        }
      }
      if (loaded && !path.empty() && oldSr > 0 && oldSr != newSr) {
        const double scale = (double)newSr / (double)oldSr;
        d.cache.open(path.c_str(), newSr);
        std::lock_guard<std::mutex> dl(d.mutex);
        d.playhead = playhead * scale;
        d.sampleRate = newSr;
        d.totalFrames = d.cache.engineFrames();
        d.flushStretcher();
        d.stretcherReady = false;
        d.stretcherSr = 0;
        d.loaded = d.totalFrames > 1;
        d.playing = wasPlaying && d.loaded;
        d.hintHot();
        d.rebuildEq((float)newSr);
        d.rebuildFilter((float)newSr);
      } else {
        std::lock_guard<std::mutex> dl(d.mutex);
        if (!d.loaded) {
          d.sampleRate = newSr;
        }
        d.rebuildEq((float)newSr);
        d.rebuildFilter((float)newSr);
      }
    }
  }

  bool tryOpenStream(int wantCh, oboe::SharingMode share, oboe::PerformanceMode perf,
                     int32_t sampleRate, int32_t deviceId) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(perf)
        ->setSharingMode(share)
        ->setFormat(oboe::AudioFormat::Float)
        ->setSampleRate(sampleRate)
        ->setUsage(oboe::Usage::Media)
        ->setSpatializationBehavior(oboe::SpatializationBehavior::Never)
        ->setChannelConversionAllowed(false)
        ->setFormatConversionAllowed(true)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::None)
        ->setChannelCount(wantCh);
    builder.setDataCallback(static_cast<oboe::AudioStreamDataCallback*>(this));
    builder.setErrorCallback(static_cast<oboe::AudioStreamErrorCallback*>(this));
    if (deviceId > 0) {
      builder.setDeviceId(deviceId);
    }
    const oboe::Result result = builder.openStream(stream);
    if (result != oboe::Result::OK) {
      LOGI("open try ch=%d share=%d perf=%d sr=%d dev=%d -> %s", wantCh, (int)share, (int)perf,
           sampleRate, deviceId, oboe::convertToText(result));
      stream.reset();
      return false;
    }
    return true;
  }

  bool openStream() {
    AtomicFlag busy(managingStream);
    std::lock_guard<std::mutex> lock(streamMutex);
    if (stream) {
      stream->stop();
      stream->close();
      stream.reset();
    }

    const int mode = outputMode.load();
    // USB 4ch is played from Java AudioTrack (channel index mask). Oboe on
    // Android USB only drives the first stereo pair and garbles the rest.
    if (mode == 1) {
      engineSampleRate.store(48000);
      engineChannels.store(4);
      applyLoadedDeckSampleRate(48000);
      LOGI("external mixer: oboe stopped, USB AudioTrack is the sink");
      return true;
    }

    bool opened =
        tryOpenStream(2, oboe::SharingMode::Exclusive, oboe::PerformanceMode::LowLatency, 48000,
                      0) ||
        tryOpenStream(2, oboe::SharingMode::Shared, oboe::PerformanceMode::LowLatency, 0, 0);
    if (!opened) {
      LOGE("openStream failed (stereo)");
      return false;
    }

    const int newSr = stream->getSampleRate();
    const int newCh = stream->getChannelCount();
    engineSampleRate.store(newSr);
    engineChannels.store(newCh);
    LOGI("stream open sr=%d ch=%d fmt=%s", newSr, newCh, oboe::convertToText(stream->getFormat()));

    applyLoadedDeckSampleRate(newSr);

    const oboe::Result result = stream->requestStart();
    return result == oboe::Result::OK;
  }

  void closeStream() {
    AtomicFlag busy(managingStream);
    std::lock_guard<std::mutex> lock(streamMutex);
    if (stream) {
      stream->stop();
      stream->close();
      stream.reset();
    }
  }

  void renderDeck(Deck& d, float* left, float* right, int frames) {
    std::unique_lock<std::mutex> lock(d.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      std::fill(left, left + frames, 0.0f);
      std::fill(right, right + frames, 0.0f);
      return;
    }
    if (!d.loaded || d.totalFrames < 2) {
      std::fill(left, left + frames, 0.0f);
      std::fill(right, right + frames, 0.0f);
      return;
    }

    const int64_t jumpTo = d.pendingJump.exchange(-1);
    bool filled = false;
    if (jumpTo >= 0 && d.playing) {
      // Old pass must not wrap: a shortened loop would snap before the xf.
      renderPlay(d, d.jumpXfL.data(), d.jumpXfR.data(), frames, false);
      d.applyJump((double)jumpTo);
      d.playing = true; // old pass can hit EOF and clear this
      renderPlay(d, left, right, frames, true);
      const float denom = (float)std::max(1, frames - 1);
      for (int i = 0; i < frames; ++i) {
        const float t = (float)i / denom;
        left[i] = d.jumpXfL[i] * (1.0f - t) + left[i] * t;
        right[i] = d.jumpXfR[i] * (1.0f - t) + right[i] * t;
      }
      filled = true;
    } else if (jumpTo >= 0) {
      d.applyJump((double)jumpTo);
    }

    if (!filled) {
      if (!d.playing && d.gate <= 0.0f) {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        d.pauseXf = false;
        return;
      }
      if (d.playing) {
        d.pauseXf = false;
        renderPlay(d, left, right, frames, true);
      } else if (!d.pauseXf) {
        // Fade out a peeked buffer; restore playhead so pause does not crawl.
        const double ph = d.playhead;
        const double rd = d.stretchRead;
        renderPlay(d, left, right, frames, true);
        d.playhead = ph;
        d.stretchRead = rd;
        d.flushStretcher();
        std::copy(left, left + frames, d.jumpXfL.begin());
        std::copy(right, right + frames, d.jumpXfR.begin());
        d.pauseXf = true;
      } else {
        std::copy(d.jumpXfL.begin(), d.jumpXfL.begin() + frames, left);
        std::copy(d.jumpXfR.begin(), d.jumpXfR.begin() + frames, right);
      }
    }

    const float step = 1.0f / 256.0f;
    for (int i = 0; i < frames; ++i) {
      if (d.playing) {
        d.gate = std::min(1.0f, d.gate + step);
      } else {
        d.gate = std::max(0.0f, d.gate - step);
      }
      left[i] *= d.gate;
      right[i] *= d.gate;
    }
  }

  void renderPlay(Deck& d, float* left, float* right, int frames, bool allowLoopWrap) {
    const int totalFrames = (int)d.totalFrames;
    const float rate = d.rate;
    const bool useKeylock = d.keylock && std::fabs(rate - 1.0f) > 0.002f;

    auto applyFx = [&](float& l, float& r) {
      const float loL = d.eqLoL.process(l);
      const float midL = d.eqMidLpL.process(d.eqMidHpL.process(l));
      const float hiL = d.eqHiL.process(l);
      l = loL * d.eqLowG + midL * d.eqMidG + hiL * d.eqHighG;
      const float loR = d.eqLoR.process(r);
      const float midR = d.eqMidLpR.process(d.eqMidHpR.process(r));
      const float hiR = d.eqHiR.process(r);
      r = loR * d.eqLowG + midR * d.eqMidG + hiR * d.eqHighG;
      l = d.filtL.process(l);
      r = d.filtR.process(r);
      const float g = d.gain * d.fader;
      l *= g;
      r *= g;
    };

    if (useKeylock) {
      d.ensureStretcher();
      if (!d.stretchPrimed) {
        d.primeStretcher(rate);
      }
      if (d.playhead >= totalFrames - 1) {
        d.playing = false;
        d.playhead = (double)std::max(0, totalFrames - 1);
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        return;
      }

      d.stretchInAccum += (double)frames * (double)rate;
      int inN = (int)d.stretchInAccum;
      d.stretchInAccum -= (double)inN;
      inN = std::max(1, std::min(inN, (int)d.stretchInL.size()));

      d.stretchRead = d.readLooped(d.stretchRead, 1.0, d.stretchInL.data(), d.stretchInR.data(),
                                   inN, allowLoopWrap);

      float* inPtrs[2] = {d.stretchInL.data(), d.stretchInR.data()};
      float* outPtrs[2] = {d.stretchOutL.data(), d.stretchOutR.data()};
      d.stretcher.process(inPtrs, inN, outPtrs, frames);

      for (int i = 0; i < frames; ++i) {
        float l = d.stretchOutL[i];
        float r = d.stretchOutR[i];
        applyFx(l, r);
        left[i] = l;
        right[i] = r;
      }
      d.playhead += (double)frames * (double)rate;
      if (allowLoopWrap && d.loopEnabled) {
        const double sr = (double)std::max(1, d.sampleRate);
        const double a = d.loopStart * sr;
        const double b = d.loopEnd * sr;
        const double len = b - a;
        if (len > 1.0e-6 && d.playhead >= b) {
          double rel = std::fmod(d.playhead - a, len);
          if (rel < 0.0) {
            rel += len;
          }
          d.playhead = a + rel;
        }
      }
      if (d.playhead >= totalFrames - 1) {
        d.playing = false;
        d.playhead = (double)std::max(0, totalFrames - 1);
      }
      d.hintHot();
      return;
    }

    d.stretchPrimed = false;

    // Keylock off: classic resample (pitch follows tempo).
    d.playhead = d.readLooped(d.playhead, (double)rate, left, right, frames, allowLoopWrap);
    for (int i = 0; i < frames; ++i) {
      applyFx(left[i], right[i]);
    }
    if (!d.loopEnabled && d.playhead >= totalFrames - 1) {
      d.playing = false;
      d.playhead = (double)std::max(0, totalFrames - 1);
    }
    d.hintHot();
  }

  void renderInterleaved(float* out, int frames, int channels) {
    if (out == nullptr || frames <= 0 || channels < 2) {
      return;
    }
    const int n = std::min(frames, kMaxCallbackFrames);
    renderDeck(decks[0], mixAL.data(), mixAR.data(), n);
    renderDeck(decks[1], mixBL.data(), mixBR.data(), n);

    const float x = xfader.load();
    float gainA = 1.0f;
    float gainB = 1.0f;
    xfaderGains(x, gainA, gainB);
    const float m = master.load();
    const bool split = outputMode.load() == 1 && channels >= 4;

    if (split) {
      for (int i = 0; i < n; ++i) {
        const int base = i * channels;
        out[base + 0] = std::max(-1.0f, std::min(1.0f, mixAL[i] * m));
        out[base + 1] = std::max(-1.0f, std::min(1.0f, mixAR[i] * m));
        out[base + 2] = std::max(-1.0f, std::min(1.0f, mixBL[i] * m));
        out[base + 3] = std::max(-1.0f, std::min(1.0f, mixBR[i] * m));
        for (int c = 4; c < channels; ++c) {
          out[base + c] = 0.0f;
        }
      }
    } else {
      for (int i = 0; i < n; ++i) {
        const float l = (mixAL[i] * gainA + mixBL[i] * gainB) * m;
        const float r = (mixAR[i] * gainA + mixBR[i] * gainB) * m;
        out[i * channels + 0] = std::max(-1.0f, std::min(1.0f, l));
        out[i * channels + 1] = std::max(-1.0f, std::min(1.0f, r));
        for (int c = 2; c < channels; ++c) {
          out[i * channels + c] = 0.0f;
        }
      }
    }
    if (frames > n) {
      std::fill(out + n * channels, out + frames * channels, 0.0f);
    }
  }

  oboe::DataCallbackResult onAudioReady(oboe::AudioStream* s, void* audioData,
                                        int32_t numFrames) override {
    if (s == nullptr || audioData == nullptr || s->getFormat() != oboe::AudioFormat::Float) {
      return oboe::DataCallbackResult::Continue;
    }
    const int ch = std::max(1, s->getChannelCount());
    engineChannels.store(ch);
    renderInterleaved(static_cast<float*>(audioData), numFrames, ch);
    return oboe::DataCallbackResult::Continue;
  }

  void onErrorAfterClose(oboe::AudioStream* /*s*/, oboe::Result error) override {
    LOGE("stream error: %s", oboe::convertToText(error));
    if (!alive.load() || managingStream.load() || outputMode.load() == 1) {
      return;
    }
    bool expected = false;
    if (!reconnecting.compare_exchange_strong(expected, true)) {
      return;
    }
    const bool ok = openStream();
    reconnecting.store(false);
    if (!ok) {
      LOGE("stream reconnect failed");
    }
  }


  static bool fileLooksLikeWav(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
      return false;
    }
    unsigned char b[12]{};
    const size_t n = std::fread(b, 1, 12, f);
    std::fclose(f);
    if (n < 12) {
      return false;
    }
    return b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' &&
           b[9] == 'A' && b[10] == 'V' && b[11] == 'E';
  }

  static bool decodeWav(const char* path, std::vector<float>& interleaved, unsigned int& channels,
                        unsigned int& sampleRate, drmp3_uint64& frames, double maxSec = 0.0) {
    drwav wav;
    if (!drwav_init_file(&wav, path, nullptr)) {
      return false;
    }
    channels = wav.channels;
    sampleRate = wav.sampleRate;
    frames = wav.totalPCMFrameCount;
    if (maxSec > 0.0 && sampleRate > 0) {
      const drmp3_uint64 lim = (drmp3_uint64)(maxSec * (double)sampleRate);
      if (lim < frames) {
        frames = lim;
      }
    }
    interleaved.resize((size_t)frames * channels);
    const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
    interleaved.resize((size_t)got * channels);
    frames = got;
    drwav_uninit(&wav);
    return frames > 0 && channels > 0;
  }

  static bool decodeMp3(const char* path, std::vector<float>& interleaved, unsigned int& channels,
                        unsigned int& sampleRate, drmp3_uint64& frames, double maxSec = 0.0) {
    if (maxSec > 0.0) {
      drmp3 mp3{};
      if (!drmp3_init_file(&mp3, path, nullptr)) {
        return false;
      }
      channels = mp3.channels;
      sampleRate = mp3.sampleRate;
      if (channels == 0 || sampleRate == 0) {
        drmp3_uninit(&mp3);
        return false;
      }
      frames = (drmp3_uint64)(maxSec * (double)sampleRate);
      interleaved.resize((size_t)frames * channels);
      const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, frames, interleaved.data());
      interleaved.resize((size_t)got * channels);
      frames = got;
      drmp3_uninit(&mp3);
      return frames > 0 && channels > 0;
    }
    drmp3_config cfg{};
    float* data = drmp3_open_file_and_read_pcm_frames_f32(path, &cfg, &frames, nullptr);
    if (!data) {
      return false;
    }
    channels = cfg.channels;
    sampleRate = cfg.sampleRate;
    interleaved.assign(data, data + frames * channels);
    drmp3_free(data, nullptr);
    return frames > 0 && channels > 0;
  }

  // First 60s, mixed to mono and resampled for BPM/key.
  static std::vector<float> decodeAnalyzeMono(const char* path, int targetSr) {
    const std::string p(path);
    std::string ext;
    if (p.size() >= 4) {
      ext = p.substr(p.size() - 4);
      for (char& c : ext) {
        c = (char)std::tolower((unsigned char)c);
      }
    }
    const bool preferWav = ext == ".wav" || fileLooksLikeWav(path);

    std::vector<float> interleaved;
    unsigned int channels = 0;
    unsigned int sampleRate = 0;
    drmp3_uint64 frames = 0;
    const double maxSec = analyze_detail::kAnalyzeWindowSec;

    bool ok = preferWav ? decodeWav(path, interleaved, channels, sampleRate, frames, maxSec)
                        : decodeMp3(path, interleaved, channels, sampleRate, frames, maxSec);
    if (!ok) {
      ok = preferWav ? decodeMp3(path, interleaved, channels, sampleRate, frames, maxSec)
                     : decodeWav(path, interleaved, channels, sampleRate, frames, maxSec);
    }
    if (!ok || frames == 0 || channels == 0) {
      return {};
    }
    return resampleMono(interleavedToMono(interleaved, channels, frames), (int)sampleRate, targetSr);
  }

  static double fileDurationSec(const char* path) {
    {
      drwav wav{};
      if (drwav_init_file(&wav, path, nullptr)) {
        const double sec = wav.sampleRate > 0
            ? (double)wav.totalPCMFrameCount / (double)wav.sampleRate
            : 0.0;
        drwav_uninit(&wav);
        if (sec > 0.0) {
          return sec;
        }
      }
    }
    drmp3 mp3{};
    if (!drmp3_init_file(&mp3, path, nullptr)) {
      return 0.0;
    }
    const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
    const double sec = mp3.sampleRate > 0 ? (double)frames / (double)mp3.sampleRate : 0.0;
    drmp3_uninit(&mp3);
    return sec;
  }
};

Engine* asEngine(DjEngine e) { return static_cast<Engine*>(e); }

} // namespace

extern "C" {

DjEngine dj_create(void) { return new Engine(); }

void dj_destroy(DjEngine engine) {
  auto* e = asEngine(engine);
  if (!e) {
    return;
  }
  e->alive.store(false);
  e->closeStream();
  delete e;
}

int dj_start(DjEngine engine) { return asEngine(engine)->openStream() ? 1 : 0; }

void dj_stop(DjEngine engine) { asEngine(engine)->closeStream(); }

int dj_set_output_mode(DjEngine engine, int mode) {
  auto* e = asEngine(engine);
  const int requested = mode == 1 ? 1 : 0;
  e->outputMode.store(requested);
  if (!e->openStream()) {
    if (requested == 1) {
      e->outputMode.store(0);
      e->openStream();
    }
    return 0;
  }
  if (requested == 1 && e->engineChannels.load() < 4) {
    LOGI("4ch requested but got %d ch; reverting to stereo", e->engineChannels.load());
    e->outputMode.store(0);
    e->openStream();
    return 0;
  }
  return 1;
}

int dj_get_output_mode(DjEngine engine) {
  return asEngine(engine)->outputMode.load();
}

int dj_get_output_channels(DjEngine engine) {
  return asEngine(engine)->engineChannels.load();
}

int dj_load(DjEngine engine, int deck, const char* path) {
  return dj_load_with_analysis(engine, deck, path, 0.0f, -1, 0.0f);
}

int dj_load_with_analysis(DjEngine engine, int deck, const char* path, float bpm, int key,
                          float beat_offset) {
  if (deck < 0 || deck > 1 || !path) {
    return 0;
  }
  auto* e = asEngine(engine);
  const int sr = e->engineSampleRate.load();

  AnalysisResult analysis;
  if (bpm > 1.0f) {
    analysis.bpm = bpm;
    analysis.key = key;
    analysis.beatOffsetSec = beat_offset;
  } else {
    const id3_meta::Tags tags = id3_meta::read(path);
    auto prefix = Engine::decodeAnalyzeMono(path, (int)analyze_detail::kAnalyzeSr);
    if (prefix.empty()) {
      return 0;
    }
    analysis =
        analyzeTrack(prefix.data(), (int)prefix.size(), 1, analyze_detail::kAnalyzeSr, tags.bpm,
                     tags.key);
  }

  Deck& d = e->decks[deck];
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.playing = false;
    d.loaded = false;
    d.pendingJump.store(-1);
    d.pendingLoopShift.store(false);
    d.gate = 0.0f;
    d.pauseXf = false;
  }
  if (!d.cache.open(path, sr)) {
    return 0;
  }
  d.cache.hintEngineFrames(0, (int64_t)sr * 2);

  std::vector<float> wmin, wmax;
  {
    StreamingDecoder scan;
    if (!scan.open(path)) {
      d.cache.stop();
      return 0;
    }
    scanWaveform(scan, kWaveformBins, wmin, wmax);
  }

  d.cache.waitChunk(0, 2500);

  std::lock_guard<std::mutex> lock(d.mutex);
  d.path = path;
  d.sampleRate = sr;
  d.channels = 2;
  d.totalFrames = d.cache.engineFrames();
  d.playhead = 0;
  d.cuePoint = 0;
  d.playing = false;
  d.loaded = d.totalFrames > 1;
  d.bpm = analysis.bpm;
  d.key = analysis.key;
  d.beatOffset = analysis.beatOffsetSec;
  d.waveMin = std::move(wmin);
  d.waveMax = std::move(wmax);
  d.flushStretcher();
  d.resetFilters();
  d.ensureStretcher();
  for (int i = 0; i < kHotCues; ++i) {
    d.hotcues[i] = -1;
  }
  d.loopEnabled = false;
  d.pendingJump.store(-1);
  d.pendingLoopShift.store(false);
  d.gate = 0.0f;
  d.pauseXf = false;
  d.rebuildEq((float)sr);
  d.rebuildFilter((float)sr);
  d.hintHot();
  LOGI("loaded deck %d frames=%lld bpm=%.2f key=%d", deck, (long long)d.totalFrames, d.bpm, d.key);
  return d.loaded ? 1 : 0;
}

int dj_analyze_file(const char* path, float* bpm, int* key, float* beat_offset) {
  if (!path) {
    return 0;
  }
  const id3_meta::Tags tags = id3_meta::read(path);
  auto pcm = Engine::decodeAnalyzeMono(path, (int)analyze_detail::kAnalyzeSr);
  if (pcm.empty()) {
    return 0;
  }
  AnalysisResult analysis =
      analyzeTrack(pcm.data(), (int)pcm.size(), 1, analyze_detail::kAnalyzeSr, tags.bpm, tags.key);
  if (bpm) {
    *bpm = analysis.bpm;
  }
  if (key) {
    *key = analysis.key;
  }
  if (beat_offset) {
    *beat_offset = analysis.beatOffsetSec;
  }
  return 1;
}

double dj_file_duration(const char* path) {
  if (!path) {
    return 0.0;
  }
  return Engine::fileDurationSec(path);
}

void dj_unload(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.loaded = false;
    d.playing = false;
    d.pendingJump.store(-1);
    d.pendingLoopShift.store(false);
    d.gate = 0.0f;
    d.pauseXf = false;
    d.waveMin.clear();
    d.waveMax.clear();
    d.totalFrames = 0;
    d.path.clear();
  }
  d.cache.stop();
}

void dj_play(DjEngine engine, int deck, int playing) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.playing = playing != 0 && d.loaded;
}

void dj_seek(DjEngine engine, int deck, double seconds) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  const double maxFrame = d.totalFrames < 2 ? 0.0 : (double)d.totalFrames - 1.0;
  const double dest =
      std::max(0.0, std::min(seconds * (double)std::max(1, d.sampleRate), maxFrame));
  d.commitJumpTo(dest);
}

void dj_set_cue(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.cuePoint = d.playhead / (double)d.sampleRate;
}

void dj_jump_cue(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.commitJumpTo(d.cuePoint * (double)d.sampleRate);
}

double dj_position(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.playhead / (double)std::max(1, d.sampleRate);
}

double dj_duration(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.totalFrames < 2 ? 0.0
                           : (double)d.totalFrames / (double)std::max(1, d.sampleRate);
}

int dj_playing(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.playing ? 1 : 0;
}

int dj_loaded(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.loaded ? 1 : 0;
}

void dj_set_gain(DjEngine engine, int deck, float linear) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.gain = std::max(0.0f, linear);
}

void dj_set_eq(DjEngine engine, int deck, float low_db, float mid_db, float high_db) {
  if (deck < 0 || deck > 1) {
    return;
  }
  auto* e = asEngine(engine);
  Deck& d = e->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.eqLow = low_db;
  d.eqMid = mid_db;
  d.eqHigh = high_db;
  d.rebuildEq((float)e->engineSampleRate.load());
}

void dj_set_filter(DjEngine engine, int deck, float amount) {
  if (deck < 0 || deck > 1) {
    return;
  }
  auto* e = asEngine(engine);
  Deck& d = e->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.filter = std::max(-1.0f, std::min(1.0f, amount));
  d.rebuildFilter((float)e->engineSampleRate.load());
}

void dj_set_fader(DjEngine engine, int deck, float linear) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.fader = clamp01(linear);
}

void dj_set_xfader(DjEngine engine, float x) {
  asEngine(engine)->xfader.store(clamp01(x));
}

void dj_set_master(DjEngine engine, float linear) {
  asEngine(engine)->master.store(std::max(0.0f, linear));
}

void dj_set_rate(DjEngine engine, int deck, float rate) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.rate = std::max(0.5f, std::min(2.0f, rate));
}

void dj_set_keylock(DjEngine engine, int deck, int enabled) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  const bool next = enabled != 0;
  if (next != d.keylock) {
    d.flushStretcher();
  }
  d.keylock = next;
}

float dj_get_bpm(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.bpm;
}

int dj_get_key(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return -1;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.key;
}

float dj_get_beat_offset(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.beatOffset;
}

void dj_nudge_grid(DjEngine engine, int deck, float seconds) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.beatOffset += seconds;
}

void dj_set_loop(DjEngine engine, int deck, int enabled, float bars) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.loopBars = std::max(0.25f, std::min(8.0f, bars));
  const bool wasOn = d.loopEnabled;
  d.loopEnabled = enabled != 0;
  if (!d.loopEnabled) {
    return;
  }
  if (wasOn) {
    d.resizeLoopFromStart();
    d.wrapPlayheadIntoLoopIfNeeded();
  } else {
    d.captureLoopAtPlayhead();
  }
}

double dj_loop_start(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.loopStart;
}

double dj_loop_end(DjEngine engine, int deck) {
  if (deck < 0 || deck > 1) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.loopEnd;
}

void dj_set_hotcue(DjEngine engine, int deck, int index, double seconds) {
  if (deck < 0 || deck > 1 || index < 0 || index >= kHotCues) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.hotcues[index] = seconds < 0 ? d.playhead / (double)d.sampleRate : seconds;
}

void dj_jump_hotcue(DjEngine engine, int deck, int index) {
  if (deck < 0 || deck > 1 || index < 0 || index >= kHotCues) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  if (d.hotcues[index] >= 0) {
    d.commitJumpTo(d.hotcues[index] * (double)d.sampleRate);
  }
}

double dj_get_hotcue(DjEngine engine, int deck, int index) {
  if (deck < 0 || deck > 1 || index < 0 || index >= kHotCues) {
    return -1;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  return d.hotcues[index];
}

void dj_clear_hotcue(DjEngine engine, int deck, int index) {
  if (deck < 0 || deck > 1 || index < 0 || index >= kHotCues) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  d.hotcues[index] = -1;
}

void dj_beat_jump(DjEngine engine, int deck, int beats) {
  if (deck < 0 || deck > 1) {
    return;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  const double delta = (60.0 / (double)std::max(1.0f, d.bpm)) * (double)beats;
  const double dest = std::max(0.0, d.playhead + delta * (double)d.sampleRate);
  d.commitJumpTo(dest, d.loopEnabled, delta);
}

int dj_waveform_bins(void) { return kWaveformBins; }

int dj_waveform_copy(DjEngine engine, int deck, float* min_out, float* max_out, int bins) {
  if (deck < 0 || deck > 1 || !min_out || !max_out || bins <= 0) {
    return 0;
  }
  Deck& d = asEngine(engine)->decks[deck];
  std::lock_guard<std::mutex> lock(d.mutex);
  const int n = std::min(bins, (int)d.waveMin.size());
  if (n <= 0) {
    return 0;
  }
  std::memcpy(min_out, d.waveMin.data(), sizeof(float) * n);
  std::memcpy(max_out, d.waveMax.data(), sizeof(float) * n);
  return n;
}

} // extern "C"

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL
Java_es_manifold_sidedeck_UsbPlayer_usbRender(JNIEnv* env, jclass, jlong handle, jobject jbuf,
                                           jint frames, jint channels) {
  if (handle == 0 || jbuf == nullptr || frames <= 0 || channels < 2) {
    return;
  }
  auto* buf = static_cast<float*>(env->GetDirectBufferAddress(jbuf));
  if (buf == nullptr) {
    return;
  }
  asEngine(reinterpret_cast<DjEngine>(handle))->renderInterleaved(buf, (int)frames, (int)channels);
}
#endif
