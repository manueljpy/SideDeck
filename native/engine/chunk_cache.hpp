#pragma once

// Decode cache: Oboe never fopen()s. A worker fills LRU chunks of native-rate
// PCM (8192 frames, ~170 ms at 48 kHz). 80 chunks ≈ 5 MB stereo.
//
// Chunks hold the file's own sample rate and are indexed by native frame.
// Rate conversion happens once in readRun() over a continuous stream, so a
// chunk boundary is never a resampling boundary and there is nothing to splice.

#include "streaming_decoder.hpp"

#include <algorithm>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define CACHE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "sidedeck.cache", __VA_ARGS__)

struct ChunkCache {
  static constexpr int kChunkFrames = 8192;
  static constexpr int kCacheChunks = 80;

  ChunkCache() {
    for (auto& s : slots_) {
      s.pcm.assign((size_t)kChunkFrames * 2, 0.0f);
    }
  }

  ChunkCache(const ChunkCache&) = delete;
  ChunkCache& operator=(const ChunkCache&) = delete;

  ~ChunkCache() { stop(); }

  // Track length in engine frames, so callers can keep working in the output
  // rate without knowing the file's rate.
  int64_t engineFrames() const {
    const int64_t native = nativeFrames_.load();
    const int nativeSr = nativeSr_.load();
    const int engineSr = engineSr_.load();
    if (native <= 0 || nativeSr < 1 || engineSr < 1) {
      return 0;
    }
    return (int64_t)((double)native * (double)engineSr / (double)nativeSr);
  }

  void setHotFrame(int64_t engineFrame) {
    hotNative_.store(toNative(engineFrame));
  }

  // On seek, drop outdated read requests so the worker does not keep
  // decoding chunks you already jumped away from.
  void jumpTo(int64_t engineFrame) {
    const int64_t native = toNative(engineFrame);
    hotNative_.store(native);
    {
      std::lock_guard<std::mutex> lock(workMu_);
      requests_.clear();
    }
    // The destination chunk first. The audio thread is already there, so
    // queueing the run-up ahead of it would make every jump wait out an extra
    // chunk decode and an extra seek before any sound comes back.
    hintNativeFrames(native, kChunkFrames * 9, true);
    // Run-up behind the destination is only wanted by a loop wrap's
    // crossfade, so it goes to the back of the queue.
    hintNativeFrames(native - kChunkFrames, kChunkFrames);
  }

  bool open(const char* path, int engineSr) {
    stop();
    if (!path || engineSr < 1) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(workMu_);
      path_ = path;
      engineSr_.store(engineSr);
      nativeSr_.store(0);
      nativeFrames_.store(0);
      stop_.store(false);
    }
    ramp_ = 0.0f;
    lastL_ = 0.0f;
    lastR_ = 0.0f;
    missFrames_.store(0);
    missEvents_.store(0);
    primeFrames_.store(0);
    primeEvents_.store(0);
    thread_ = std::thread([this]() { workerMain(); });
    // Wait until the worker has opened the file (or failed).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (opened_.load()) {
        return engineFrames() > 0;
      }
      if (openFailed_.load()) {
        stop();
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop();
    return false;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(workMu_);
      stop_.store(true);
      path_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    stop_.store(false);
    opened_.store(false);
    openFailed_.store(false);
    {
      std::lock_guard<std::mutex> lock(cacheMu_);
      index_.clear();
      lru_.clear();
      for (auto& s : slots_) {
        s.chunkIndex = -1;
        s.ready = false;
      }
    }
    {
      std::lock_guard<std::mutex> lock(workMu_);
      requests_.clear();
    }
  }

  // A range worth keeping decoded, in engine frames.
  struct Span {
    int64_t start = 0;
    int64_t count = 0;
  };

  // Prefetch a span given in engine frames. Callers think in the output rate;
  // the chunks underneath are native, so convert before queueing.
  void hintEngineFrames(int64_t start, int64_t count, bool urgent = false) {
    const Span span{start, count};
    hintEngineSpans(&span, 1, urgent);
  }

  // Prefetch several spans at once. The audio thread re-hints every anchor on
  // every buffer, so it takes the locks once for the batch rather than once
  // per anchor.
  void hintEngineSpans(const Span* spans, int n, bool urgent = false) {
    const int64_t total = nativeFrames_.load();
    if (!spans || n < 1 || total <= 0) {
      return;
    }
    const double ratio = nativeRatio();
    bool wake = false;
    {
      std::lock_guard<std::mutex> work(workMu_);
      std::lock_guard<std::mutex> cached(cacheMu_);
      for (int i = 0; i < n; ++i) {
        if (spans[i].count <= 0) {
          continue;
        }
        const int64_t start = (int64_t)((double)spans[i].start * ratio);
        const int64_t count =
            std::max<int64_t>(1, (int64_t)((double)spans[i].count * ratio));
        queueSpanUnlocked(start, count, total, urgent, wake);
      }
      trimRequestsUnlocked();
    }
    if (wake) {
      cv_.notify_one();
    }
  }

  bool waitChunk(int chunkIndex, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (hasChunk(chunkIndex)) {
        return true;
      }
      hintNativeFrames((int64_t)chunkIndex * kChunkFrames, kChunkFrames);
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return hasChunk(chunkIndex);
  }

  // Run of n frames starting at engine frame pos, advancing by step engine
  // frames each. Resamples from the native chunks on the way out, reading the
  // neighbouring frame across a chunk edge so the interpolation stays
  // continuous. Never blocks: a miss decays the last good sample and then
  // ramps back in, so the first decoded block does not click against zeros.
  // Locks once for the whole run, so slot pointers stay valid and the audio
  // thread does not take a mutex per sample.
  // `priming` marks a speculative lookahead read (the stretcher seeking after
  // a jump) rather than frames on their way to the speaker. It reads exactly
  // the same, but is counted apart: one prime read is thousands of frames, and
  // folding that into the playback figure makes a healthy jump look like a
  // dropout.
  void readRun(double pos, double step, float* L, float* R, int n,
               bool priming = false) {
    const int64_t total = nativeFrames_.load();
    const double ratio = nativeRatio();
    int64_t missAt = -1;
    int missed = 0;
    {
      std::lock_guard<std::mutex> lock(cacheMu_);
      int heldChunk = -1;
      const float* held = nullptr;
      for (int k = 0; k < n; ++k) {
        // Convert each engine position on its own rather than accumulating a
        // native step, so the run matches a resample of the whole file exactly.
        const double p = (pos + (double)k * step) * ratio;
        if (total < 2 || p < 0.0 || p >= (double)total - 1.0) {
          L[k] = R[k] = 0.0f;
          continue;
        }
        const int64_t i0 = (int64_t)p;
        const int chunk = (int)(i0 / kChunkFrames);
        if (chunk != heldChunk) {
          heldChunk = chunk;
          held = chunkDataUnlocked(chunk);
        }
        if (held == nullptr) {
          // Decay the last good sample rather than stepping to zero. A miss
          // is normal while the worker catches up, and a step is a click.
          ramp_ = std::max(0.0f, ramp_ - kMissFadeStep);
          L[k] = lastL_ * ramp_;
          R[k] = lastR_ * ramp_;
          if (missAt < 0) {
            missAt = i0;
          }
          ++missed;
          continue;
        }
        const int off = (int)(i0 - (int64_t)chunk * kChunkFrames);
        const float l0 = held[off * 2];
        const float r0 = held[off * 2 + 1];
        float l1 = l0;
        float r1 = r0;
        if (off + 1 < kChunkFrames) {
          l1 = held[(off + 1) * 2];
          r1 = held[(off + 1) * 2 + 1];
        } else if (const float* next = chunkDataUnlocked(chunk + 1)) {
          l1 = next[0];
          r1 = next[1];
        }
        const float frac = (float)(p - (double)i0);
        const float l = l0 * (1.0f - frac) + l1 * frac;
        const float r = r0 * (1.0f - frac) + r1 * frac;
        lastL_ = l;
        lastR_ = r;
        if (ramp_ < 1.0f) {
          ramp_ = std::min(1.0f, ramp_ + kMissFadeStep);
        }
        L[k] = l * ramp_;
        R[k] = r * ramp_;
      }
    }
    hotNative_.store((int64_t)(pos * ratio));
    // Outside cacheMu_: hintNativeFrames takes workMu_ and cacheMu_.
    if (missed > 0) {
      // Count rather than just hide it. A miss during steady playback means
      // the worker is losing to the audio thread, and the fade above would
      // otherwise make that inaudible and undiagnosable.
      if (priming) {
        primeFrames_.fetch_add((uint64_t)missed, std::memory_order_relaxed);
        primeEvents_.fetch_add(1, std::memory_order_relaxed);
      } else {
        missFrames_.fetch_add((uint64_t)missed, std::memory_order_relaxed);
        missEvents_.fetch_add(1, std::memory_order_relaxed);
      }
      hintNativeFrames(missAt, kChunkFrames * 4, true);
    }
  }

 private:
  static constexpr float kMissFadeStep = 1.0f / 256.0f;
  static constexpr size_t kMaxRequests = 20;
  // Android's THREAD_PRIORITY_AUDIO. Decoding feeds a realtime callback, so
  // it has to outrank the UI and raster threads it shares the CPU with.
  static constexpr int kWorkerNice = -16;

  struct Slot {
    int chunkIndex = -1;
    bool ready = false;
    std::vector<float> pcm;
  };

  double nativeRatio() const {
    const int nativeSr = nativeSr_.load();
    const int engineSr = engineSr_.load();
    if (nativeSr < 1 || engineSr < 1) {
      return 1.0;
    }
    return (double)nativeSr / (double)engineSr;
  }

  int64_t toNative(int64_t engineFrame) const {
    return (int64_t)((double)engineFrame * nativeRatio());
  }

  void hintNativeFrames(int64_t start, int64_t count, bool urgent = false) {
    const int64_t total = nativeFrames_.load();
    if (total <= 0 || count <= 0) {
      return;
    }
    bool wake = false;
    {
      std::lock_guard<std::mutex> work(workMu_);
      std::lock_guard<std::mutex> cached(cacheMu_);
      queueSpanUnlocked(start, count, total, urgent, wake);
      trimRequestsUnlocked();
    }
    if (wake) {
      cv_.notify_one();
    }
  }

  // Both workMu_ and cacheMu_ held, in that order.
  void queueSpanUnlocked(int64_t start, int64_t count, int64_t total, bool urgent,
                         bool& wake) {
    const int64_t a = std::max<int64_t>(0, start);
    if (a >= total) {
      return;
    }
    const int64_t b = std::min(total, start + count);
    const int first = (int)(a / kChunkFrames);
    const int last = (int)((std::max(a, b - 1)) / kChunkFrames);
    if (urgent) {
      // Nearest first. Walk backwards so `first` is at the front after each
      // push_front.
      for (int i = last; i >= first; --i) {
        queueChunkUnlocked(i, true, wake);
      }
    } else {
      for (int i = first; i <= last; ++i) {
        queueChunkUnlocked(i, false, wake);
      }
    }
  }

  void queueChunkUnlocked(int chunkIndex, bool front, bool& wake) {
    if (hasChunkUnlocked(chunkIndex)) {
      return;
    }
    auto it = std::find(requests_.begin(), requests_.end(), chunkIndex);
    if (front) {
      if (it != requests_.end()) {
        requests_.erase(it);
      }
      requests_.push_front(chunkIndex);
    } else if (it != requests_.end()) {
      return;
    } else {
      requests_.push_back(chunkIndex);
    }
    wake = true;
  }

  // Cap in-flight reads. Drop the farthest (back), never the playhead.
  void trimRequestsUnlocked() {
    while (requests_.size() > kMaxRequests) {
      requests_.pop_back();
    }
  }

  bool hasChunk(int chunkIndex) {
    std::lock_guard<std::mutex> lock(cacheMu_);
    return hasChunkUnlocked(chunkIndex);
  }

  bool hasChunkUnlocked(int chunkIndex) const {
    auto it = index_.find(chunkIndex);
    return it != index_.end() && slots_[it->second].ready;
  }

  const float* chunkDataUnlocked(int chunkIndex) const {
    auto it = index_.find(chunkIndex);
    if (it == index_.end() || !slots_[it->second].ready) {
      return nullptr;
    }
    return slots_[it->second].pcm.data();
  }

  void touchLruUnlocked(int slot) {
    lru_.erase(std::remove(lru_.begin(), lru_.end(), slot), lru_.end());
    lru_.push_back(slot);
  }

  int allocateSlotUnlocked(int chunkIndex) {
    auto existing = index_.find(chunkIndex);
    if (existing != index_.end()) {
      return existing->second;
    }
    int slot = -1;
    for (int i = 0; i < kCacheChunks; ++i) {
      if (slots_[i].chunkIndex < 0) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      const int hot = (int)(hotNative_.load() / kChunkFrames);
      int victim = -1;
      for (int s : lru_) {
        const int ci = slots_[s].chunkIndex;
        if (ci >= hot - 1 && ci <= hot + 8) {
          continue;  // do not expire the playhead window
        }
        victim = s;
        break;
      }
      if (victim < 0) {
        victim = lru_.empty() ? 0 : lru_.front();
      }
      lru_.erase(std::remove(lru_.begin(), lru_.end(), victim), lru_.end());
      slot = victim;
      index_.erase(slots_[slot].chunkIndex);
    }
    slots_[slot].chunkIndex = chunkIndex;
    slots_[slot].ready = false;
    index_[chunkIndex] = slot;
    return slot;
  }

  void workerMain() {
    // Best effort: if the platform refuses the priority bump, decoding still
    // works, it just has less headroom against a busy UI thread.
    setpriority(PRIO_PROCESS, (id_t)gettid(), kWorkerNice);
    StreamingDecoder dec;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(workMu_);
      path = path_;
    }
    if (!dec.open(path.c_str())) {
      openFailed_.store(true);
      return;
    }
    nativeSr_.store((int)dec.sampleRate);
    nativeFrames_.store((int64_t)dec.totalFrames);
    opened_.store(true);

    uint32_t loggedMissEvents = 0;
    uint32_t loggedPrimeEvents = 0;
    auto lastMissLog = std::chrono::steady_clock::now();

    while (!stop_.load()) {
      int chunkIndex = -1;
      {
        std::unique_lock<std::mutex> lock(workMu_);
        cv_.wait(lock, [&]() { return stop_.load() || !requests_.empty(); });
        if (stop_.load()) {
          break;
        }
        chunkIndex = requests_.front();
        requests_.pop_front();
      }

      // Report misses from the worker, not the audio thread, so the logging
      // itself never blocks playback.
      const uint32_t misses = missEvents_.load(std::memory_order_relaxed);
      const uint32_t primes = primeEvents_.load(std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();
      if ((misses != loggedMissEvents || primes != loggedPrimeEvents) &&
          now - lastMissLog >= std::chrono::seconds(1)) {
        CACHE_LOGI("miss since load: playback %u buffers/%llu frames, "
                   "jump-prime %u reads/%llu frames",
                   misses, (unsigned long long)missFrames_.load(std::memory_order_relaxed),
                   primes, (unsigned long long)primeFrames_.load(std::memory_order_relaxed));
        loggedMissEvents = misses;
        loggedPrimeEvents = primes;
        lastMissLog = now;
      }

      if (hasChunk(chunkIndex)) {
        // Duplicate of an in-flight/ready chunk. Keep the decoder cursor so
        // the next uncached neighbor can stay sequential.
        continue;
      }
      std::vector<float> filled;
      if (!decodeChunk(dec, chunkIndex, filled)) {
        continue;
      }
      std::lock_guard<std::mutex> lock(cacheMu_);
      const int slot = allocateSlotUnlocked(chunkIndex);
      slots_[slot].pcm.swap(filled);
      if ((int)slots_[slot].pcm.size() < kChunkFrames * 2) {
        slots_[slot].pcm.resize((size_t)kChunkFrames * 2, 0.0f);
      }
      slots_[slot].ready = true;
      touchLruUnlocked(slot);
    }
  }

  // Move the decoder cursor to a native frame. Staying sequential is both
  // cheaper and more accurate than re-entering the stream, so decode forward
  // whenever the target is close and ahead.
  bool seekNativeTo(StreamingDecoder& dec, int64_t nativeStart) {
    constexpr int64_t kMp3Frame = 1152;
    const bool isMp3 = dec.kind == StreamingDecoder::Kind::Mp3;
    const int64_t maxSkip = isMp3 ? 29 * kMp3Frame : (int64_t)8192;
    const int64_t cur = (int64_t)dec.tell();
    if (cur == nativeStart) {
      return true;
    }
    if (cur >= 0 && cur < nativeStart && nativeStart - cur <= maxSkip) {
      std::vector<float> skip((size_t)(nativeStart - cur) * 2);
      dec.readStereo(skip.data(), (uint64_t)(nativeStart - cur));
      return true;
    }
    // Run-up before the target. With a seek table bound, dr_mp3 rebuilds the
    // bit reservoir itself, so a few frames of margin is enough: 511 bytes of
    // reservoir backreference is under two MP3 frames at any bitrate, plus one
    // for the synthesis filterbank overlap. Without a table a seek restarts
    // the file anyway, so come in from much further back.
    int64_t preroll = 0;
    if (isMp3) {
      preroll = dec.hasSeekTable() ? 4 * kMp3Frame : 29 * kMp3Frame;
    }
    const int64_t restart = std::max<int64_t>(0, nativeStart - preroll);
    if (!dec.seek((uint64_t)restart)) {
      return false;
    }
    const int64_t skipN = nativeStart - restart;
    if (skipN > 0) {
      std::vector<float> skip((size_t)skipN * 2);
      dec.readStereo(skip.data(), (uint64_t)skipN);
    }
    return true;
  }

  // A chunk is just the file's own frames, so decoding it is a seek plus a
  // read. Sequential requests leave the cursor exactly on the next chunk's
  // first frame, so the common case costs no seek at all.
  bool decodeChunk(StreamingDecoder& dec, int chunkIndex, std::vector<float>& out) {
    out.assign((size_t)kChunkFrames * 2, 0.0f);
    const int64_t nativeStart = (int64_t)chunkIndex * kChunkFrames;
    if (nativeStart >= (int64_t)dec.totalFrames) {
      return true;
    }
    if (!seekNativeTo(dec, nativeStart)) {
      return false;
    }
    // A short read near EOF leaves zeros, which readRun() never reaches: it
    // stops at nativeFrames_.
    dec.readStereo(out.data(), (uint64_t)kChunkFrames);
    return true;
  }

  Slot slots_[kCacheChunks];
  std::unordered_map<int, int> index_;
  std::deque<int> lru_;
  std::mutex cacheMu_;

  std::mutex workMu_;
  std::condition_variable cv_;
  std::deque<int> requests_;
  std::thread thread_;
  std::string path_;
  std::atomic<int> engineSr_{48000};
  std::atomic<int> nativeSr_{0};
  std::atomic<int64_t> nativeFrames_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> opened_{false};
  std::atomic<bool> openFailed_{false};
  std::atomic<int64_t> hotNative_{0};
  std::atomic<uint64_t> missFrames_{0};
  std::atomic<uint32_t> missEvents_{0};
  std::atomic<uint64_t> primeFrames_{0};
  std::atomic<uint32_t> primeEvents_{0};
  float ramp_ = 1.0f;
  float lastL_ = 0.0f;
  float lastR_ = 0.0f;
};
