#pragma once

// Decode cache: Oboe never fopen()s. A worker fills LRU chunks (~170 ms at
// 48 kHz). 80 chunks ≈ 5 MB stereo.

#include "streaming_decoder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

  int64_t engineFrames() const { return engineFrames_.load(); }

  void setHotFrame(int64_t frame) { hotFrame_.store(frame); }

  // On seek, drop outdated read requests so the worker does not keep
  // decoding chunks you already jumped away from.
  void jumpTo(int64_t frame) {
    hotFrame_.store(frame);
    ramp_ = 1.0f;
    wasMiss_.store(false);
    {
      std::lock_guard<std::mutex> lock(workMu_);
      requests_.clear();
    }
    hintEngineFrames(frame - kChunkFrames, kChunkFrames * 10, true);
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
      engineFrames_.store(0);
      stop_.store(false);
    }
    thread_ = std::thread([this]() { workerMain(); });
    // Wait until the worker has opened the file (or failed).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (opened_.load()) {
        return engineFrames_.load() > 0;
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

  void hintEngineFrames(int64_t start, int64_t count, bool urgent = false) {
    const int64_t total = engineFrames_.load();
    if (total <= 0 || count <= 0) {
      return;
    }
    const int64_t a = std::max<int64_t>(0, start);
    const int64_t b = std::min(total, start + count);
    const int first = (int)(a / kChunkFrames);
    const int last = (int)((std::max(a, b - 1)) / kChunkFrames);
    bool wake = false;
    {
      std::lock_guard<std::mutex> lock(workMu_);
      if (urgent) {
        // Playhead first. Walk backwards so `first` is at the front after
        // each push_front.
        for (int i = last; i >= first; --i) {
          if (hasChunk(i)) {
            continue;
          }
          auto it = std::find(requests_.begin(), requests_.end(), i);
          if (it != requests_.end()) {
            requests_.erase(it);
          }
          requests_.push_front(i);
          wake = true;
        }
      } else {
        for (int i = first; i <= last; ++i) {
          if (hasChunk(i)) {
            continue;
          }
          if (std::find(requests_.begin(), requests_.end(), i) != requests_.end()) {
            continue;
          }
          requests_.push_back(i);
          wake = true;
        }
      }
      // Cap in-flight reads. Drop the farthest (back), never the playhead.
      while (requests_.size() > 20) {
        requests_.pop_back();
      }
    }
    if (wake) {
      cv_.notify_one();
    }
  }

  bool waitAround(int64_t frame, int64_t count, int timeoutMs) {
    if (count < kChunkFrames) {
      count = kChunkFrames;
    }
    hintEngineFrames(frame - kChunkFrames / 4, count + kChunkFrames, true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const int64_t total = engineFrames_.load();
    const int64_t a = std::max<int64_t>(0, frame);
    const int64_t b = std::min(total, frame + count);
    if (b <= a) {
      return true;
    }
    const int first = (int)(a / kChunkFrames);
    const int last = (int)((b - 1) / kChunkFrames);
    for (int i = first; i <= last; ++i) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) {
        break;
      }
      waitChunk(i, (int)left);
    }
    return hasChunk(first);
  }

  bool waitChunk(int chunkIndex, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (hasChunk(chunkIndex)) {
        return true;
      }
      hintEngineFrames((int64_t)chunkIndex * kChunkFrames, kChunkFrames);
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return hasChunk(chunkIndex);
  }

  // Engine-rate playhead. Never blocks. Miss = silence, then ramp back in
  // so the first decoded block does not click against zeros.
  void readPcm(double pos, float& l, float& r) {
    const int64_t total = engineFrames_.load();
    if (total < 2 || pos < 0.0 || pos >= (double)total - 1.0) {
      l = r = 0.0f;
      return;
    }
    hotFrame_.store((int64_t)pos);
    const int i0 = (int)pos;
    const int i1 = i0 + 1;
    const float frac = (float)(pos - (double)i0);
    float l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    const bool ok0 = sampleAt(i0, l0, r0);
    const bool ok1 = sampleAt(i1, l1, r1);
    if (!ok0) {
      l = r = 0.0f;
      wasMiss_.store(true);
      ramp_ = 0.0f;
      hintEngineFrames(i0, kChunkFrames * 4, true);
      return;
    }
    if (!ok1) {
      l1 = l0;
      r1 = r0;
    }
    l = l0 * (1.0f - frac) + l1 * frac;
    r = r0 * (1.0f - frac) + r1 * frac;
    if (wasMiss_.exchange(false)) {
      ramp_ = 0.0f;
    }
    if (ramp_ < 1.0f) {
      ramp_ = std::min(1.0f, ramp_ + (1.0f / 256.0f));
      l *= ramp_;
      r *= ramp_;
    }
  }

 private:
  struct Slot {
    int chunkIndex = -1;
    bool ready = false;
    std::vector<float> pcm;
  };

  bool hasChunk(int chunkIndex) {
    std::lock_guard<std::mutex> lock(cacheMu_);
    return hasChunkUnlocked(chunkIndex);
  }

  bool hasChunkUnlocked(int chunkIndex) const {
    auto it = index_.find(chunkIndex);
    return it != index_.end() && slots_[it->second].ready;
  }

  bool sampleAt(int engineFrame, float& l, float& r) {
    const int chunkIndex = engineFrame / kChunkFrames;
    const int off = engineFrame - chunkIndex * kChunkFrames;
    std::lock_guard<std::mutex> lock(cacheMu_);
    auto it = index_.find(chunkIndex);
    if (it == index_.end() || !slots_[it->second].ready) {
      return false;
    }
    const float* p = slots_[it->second].pcm.data();
    l = p[off * 2];
    r = p[off * 2 + 1];
    return true;
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
      const int hot = (int)(hotFrame_.load() / kChunkFrames);
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
    const int engineSr = engineSr_.load();
    engineFrames_.store(dec.engineFrameCount(engineSr));
    opened_.store(true);
    int64_t decoderNative = -1;

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
      if (hasChunk(chunkIndex)) {
        // Duplicate of an in-flight/ready chunk. Keep the decoder cursor so
        // the next uncached neighbor can stay sequential.
        continue;
      }
      std::vector<float> filled;
      if (!decodeChunk(dec, engineSr, chunkIndex, decoderNative, filled)) {
        decoderNative = -1;
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

  bool decodeChunk(StreamingDecoder& dec, int engineSr, int chunkIndex,
                   int64_t& decoderNative, std::vector<float>& out) {
    out.assign((size_t)kChunkFrames * 2, 0.0f);
    if (dec.sampleRate < 1 || engineSr < 1) {
      return false;
    }
    const int64_t engineStart = (int64_t)chunkIndex * kChunkFrames;
    const double ratio = (double)dec.sampleRate / (double)engineSr;
    const int64_t nativeStart = (int64_t)(engineStart * ratio);
    const int64_t nextNativeStart =
        (int64_t)((engineStart + kChunkFrames) * ratio);
    if (nativeStart >= (int64_t)dec.totalFrames) {
      decoderNative = nativeStart;
      return true;
    }
    // If the cursor is already before the target and close, keep decoding
    // (MP3 bit reservoir stays valid). A small backward drmp3_seek()
    // restarts the whole file.
    constexpr int64_t kMp3Preroll = 29 * 1152;
    const int64_t maxSkip = dec.kind == StreamingDecoder::Kind::Mp3 ? kMp3Preroll : (int64_t)8192;
    decoderNative = (int64_t)dec.tell();
    if (decoderNative == nativeStart) {
      // already there
    } else if (decoderNative >= 0 && decoderNative < nativeStart &&
               nativeStart - decoderNative <= maxSkip) {
      std::vector<float> skip((size_t)(nativeStart - decoderNative) * 2);
      dec.readStereo(skip.data(), (uint64_t)(nativeStart - decoderNative));
    } else if (decoderNative > nativeStart && decoderNative - nativeStart <= 8) {
      if (!dec.seek((uint64_t)nativeStart)) {
        return false;
      }
    } else {
      const int64_t restart =
          dec.kind == StreamingDecoder::Kind::Mp3
              ? std::max<int64_t>(0, nativeStart - kMp3Preroll)
              : nativeStart;
      if (!dec.seek((uint64_t)restart)) {
        return false;
      }
      const int64_t skipN = nativeStart - restart;
      if (skipN > 0) {
        std::vector<float> skip((size_t)skipN * 2);
        dec.readStereo(skip.data(), (uint64_t)skipN);
      }
    }
    decoderNative = (int64_t)dec.tell();
    const int64_t want = std::max<int64_t>(1, nextNativeStart - nativeStart);
    std::vector<float> native((size_t)want * 2, 0.0f);
    const uint64_t got = dec.readStereo(native.data(), (uint64_t)want);
    decoderNative = (int64_t)dec.tell();
    if (got == 0) {
      return true;
    }
    const int nativeFrames = (int)got;
    for (int i = 0; i < kChunkFrames; ++i) {
      const double np = (engineStart + i) * ratio - (double)nativeStart;
      int i0 = (int)np;
      if (i0 < 0) {
        i0 = 0;
      }
      if (i0 >= nativeFrames) {
        break;
      }
      const int i1 = std::min(i0 + 1, nativeFrames - 1);
      const float frac = (float)(np - (double)i0);
      out[(size_t)i * 2] =
          native[(size_t)i0 * 2] * (1.0f - frac) + native[(size_t)i1 * 2] * frac;
      out[(size_t)i * 2 + 1] =
          native[(size_t)i0 * 2 + 1] * (1.0f - frac) + native[(size_t)i1 * 2 + 1] * frac;
    }
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
  std::atomic<int64_t> engineFrames_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> opened_{false};
  std::atomic<bool> openFailed_{false};
  std::atomic<int64_t> hotFrame_{0};
  std::atomic<bool> wasMiss_{false};
  float ramp_ = 1.0f;
};
