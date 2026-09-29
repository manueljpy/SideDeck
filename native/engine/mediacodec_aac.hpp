#pragma once

// AAC-in-M4A (and ADTS .aac) via Android NDK MediaExtractor + MediaCodec.
// Same approach as Algoriddim djay: OS decoder for AAC, no patented codec in the APK.

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#ifndef AMEDIAFORMAT_KEY_PCM_ENCODING
#define AMEDIAFORMAT_KEY_PCM_ENCODING "pcm-encoding"
#endif
#ifndef AMEDIAFORMAT_KEY_ENCODER_DELAY
#define AMEDIAFORMAT_KEY_ENCODER_DELAY "encoder-delay"
#endif
#ifndef AMEDIAFORMAT_KEY_ENCODER_PADDING
#define AMEDIAFORMAT_KEY_ENCODER_PADDING "encoder-padding"
#endif

// Matches AudioFormat.ENCODING_PCM_* when KEY_PCM_ENCODING is present.
#ifndef kAudioFormatPcm16bit
constexpr int32_t kAudioFormatPcm16bit = 2;
constexpr int32_t kAudioFormatPcmFloat = 4;
constexpr int32_t kAudioFormatPcm8bit = 3;
constexpr int32_t kAudioFormatPcm24bitPacked = 21;
constexpr int32_t kAudioFormatPcm32bit = 22;
#endif

struct MediaCodecAacDecoder {
  AMediaExtractor* extractor = nullptr;
  AMediaCodec* codec = nullptr;
  int fd = -1;
  unsigned channels = 0;
  unsigned sampleRate = 0;
  uint64_t totalFrames = 0;
  uint64_t pcmCursor = 0;
  int32_t pcmEncoding = kAudioFormatPcm16bit;
  bool inputEos = false;
  bool outputEos = false;

  // Decoded stereo float not yet consumed by readStereo.
  std::vector<float> pending;
  size_t pendingRead = 0;

  MediaCodecAacDecoder() = default;
  MediaCodecAacDecoder(const MediaCodecAacDecoder&) = delete;
  MediaCodecAacDecoder& operator=(const MediaCodecAacDecoder&) = delete;
  ~MediaCodecAacDecoder() { close(); }

  void close() {
    if (codec) {
      AMediaCodec_stop(codec);
      AMediaCodec_delete(codec);
      codec = nullptr;
    }
    if (extractor) {
      AMediaExtractor_delete(extractor);
      extractor = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    channels = 0;
    sampleRate = 0;
    totalFrames = 0;
    pcmCursor = 0;
    pcmEncoding = kAudioFormatPcm16bit;
    inputEos = false;
    outputEos = false;
    pending.clear();
    pendingRead = 0;
  }

  static bool mimeIsAac(const char* mime) {
    if (!mime) {
      return false;
    }
    return std::strcmp(mime, "audio/mp4a-latm") == 0 || std::strcmp(mime, "audio/mpeg-L4") == 0 ||
           std::strcmp(mime, "audio/aac") == 0;
  }

  static bool mimeIsAlac(const char* mime) {
    if (!mime) {
      return false;
    }
    return std::strcmp(mime, "audio/alac") == 0 || std::strcmp(mime, "audio/x-alac") == 0 ||
           std::strcmp(mime, "audio/mp4a-alac") == 0;
  }

  bool open(const char* path) {
    close();
    if (!path || !path[0]) {
      return false;
    }

    fd = ::open(path, O_RDONLY);
    if (fd < 0) {
      return false;
    }
    const off_t len = ::lseek(fd, 0, SEEK_END);
    if (len <= 0) {
      close();
      return false;
    }
    ::lseek(fd, 0, SEEK_SET);

    extractor = AMediaExtractor_new();
    if (!extractor) {
      close();
      return false;
    }
    if (AMediaExtractor_setDataSourceFd(extractor, fd, 0, (off64_t)len) != AMEDIA_OK) {
      close();
      return false;
    }

    const size_t trackCount = AMediaExtractor_getTrackCount(extractor);
    size_t audioTrack = (size_t)-1;
    AMediaFormat* format = nullptr;
    const char* mime = nullptr;
    for (size_t i = 0; i < trackCount; ++i) {
      AMediaFormat* f = AMediaExtractor_getTrackFormat(extractor, i);
      if (!f) {
        continue;
      }
      const char* m = nullptr;
      if (!AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) || !m) {
        AMediaFormat_delete(f);
        continue;
      }
      if (std::strncmp(m, "audio/", 6) != 0) {
        AMediaFormat_delete(f);
        continue;
      }
      if (mimeIsAlac(m)) {
        AMediaFormat_delete(f);
        __android_log_print(ANDROID_LOG_WARN, "sidedeck",
                            "m4a/ALAC is not supported (AAC only): %s", path);
        close();
        return false;
      }
      if (mimeIsAac(m)) {
        audioTrack = i;
        format = f;
        mime = m;
        break;
      }
      AMediaFormat_delete(f);
    }
    if (audioTrack == (size_t)-1 || !format || !mime) {
      if (format) {
        AMediaFormat_delete(format);
      }
      close();
      return false;
    }

    // Absurd encoder-delay values (common on ffmpeg AAC under Android 9+) make
    // the decoder emit silence / empty buffers. Cap to a realistic AAC delay.
    int32_t encDelay = 0;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_ENCODER_DELAY, &encDelay) &&
        encDelay > 4096) {
      AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_ENCODER_DELAY, 0);
    }
    int32_t encPad = 0;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_ENCODER_PADDING, &encPad) &&
        encPad > 4096) {
      AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_ENCODER_PADDING, 0);
    }

    int32_t ch = 0;
    int32_t sr = 0;
    int64_t durationUs = 0;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
    AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &durationUs);
    if (ch < 1 || sr < 1 || durationUs <= 0) {
      AMediaFormat_delete(format);
      close();
      return false;
    }

    AMediaExtractor_selectTrack(extractor, audioTrack);

    codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) {
      AMediaFormat_delete(format);
      close();
      return false;
    }
    if (AMediaCodec_configure(codec, format, nullptr, nullptr, 0) != AMEDIA_OK) {
      AMediaFormat_delete(format);
      close();
      return false;
    }
    AMediaFormat_delete(format);
    format = nullptr;
    if (AMediaCodec_start(codec) != AMEDIA_OK) {
      close();
      return false;
    }

    channels = (unsigned)ch;
    sampleRate = (unsigned)sr;
    totalFrames = (uint64_t)((double)durationUs * (double)sampleRate / 1000000.0 + 0.5);
    if (totalFrames == 0) {
      close();
      return false;
    }
    pcmCursor = 0;
    inputEos = false;
    outputEos = false;
    pending.clear();
    pendingRead = 0;
    return true;
  }

  uint64_t tell() const { return pcmCursor; }

  bool seek(uint64_t frame) {
    if (!extractor || !codec || sampleRate == 0) {
      return false;
    }
    if (frame > totalFrames) {
      frame = totalFrames;
    }
    const int64_t targetUs =
        (int64_t)std::llround((double)frame * 1000000.0 / (double)sampleRate);
    if (AMediaExtractor_seekTo(extractor, targetUs, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) !=
        AMEDIA_OK) {
      return false;
    }
    AMediaCodec_flush(codec);
    pending.clear();
    pendingRead = 0;
    inputEos = false;
    outputEos = false;

    // sampleTime is 0 at t=0; <0 means no current sample after seek.
    const int64_t sampleTime = AMediaExtractor_getSampleTime(extractor);
    uint64_t approx = 0;
    if (sampleTime < 0) {
      if (frame > 0) {
        // Extractor/codec already moved; reset to a consistent start before failing.
        resetToStartAfterFailedSeek();
        return false;
      }
    } else {
      approx = (uint64_t)((double)sampleTime * (double)sampleRate / 1000000.0);
    }
    pcmCursor = approx;
    // Must land at or before the target so callers (chunk cache) can discard
    // forward to the exact frame. Past-target means we can't recover by reading.
    if (pcmCursor > frame) {
      resetToStartAfterFailedSeek();
      return false;
    }
    if (pcmCursor == frame) {
      return true;
    }
    // Discard forward in fixed chunks so a bad approx cannot OOM.
    constexpr uint64_t kDiscardChunk = 4096;
    std::vector<float> dump((size_t)kDiscardChunk * 2);
    while (pcmCursor < frame) {
      const uint64_t need = frame - pcmCursor;
      const uint64_t n = std::min(need, kDiscardChunk);
      const uint64_t got = readStereo(dump.data(), n);
      if (got < n) {
        // readStereo advanced pcmCursor honestly; leave decoder there.
        return false;
      }
    }
    return pcmCursor == frame;
  }

  uint64_t readStereo(float* out, uint64_t frames) {
    if (!out || frames == 0 || !codec || channels == 0) {
      return 0;
    }
    uint64_t got = 0;
    while (got < frames) {
      drainPending(out + got * 2, frames - got, got);
      if (got >= frames) {
        break;
      }
      // Drain any leftover PCM after EOS before giving up.
      if (outputEos) {
        break;
      }
      if (!pump()) {
        break;
      }
    }
    return got;
  }

 private:
  // After a failed mid-file seekTo, extractor/codec may already be moved while
  // pcmCursor still reflects the old position. Snap back to t=0 so tell() matches.
  void resetToStartAfterFailedSeek() {
    if (extractor) {
      AMediaExtractor_seekTo(extractor, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    }
    if (codec) {
      AMediaCodec_flush(codec);
    }
    pending.clear();
    pendingRead = 0;
    inputEos = false;
    outputEos = false;
    pcmCursor = 0;
  }

  void drainPending(float* out, uint64_t want, uint64_t& got) {
    const size_t available = pending.size() / 2 - pendingRead;
    const size_t n = (size_t)std::min<uint64_t>(want, (uint64_t)available);
    for (size_t i = 0; i < n; ++i) {
      out[i * 2] = pending[(pendingRead + i) * 2];
      out[i * 2 + 1] = pending[(pendingRead + i) * 2 + 1];
    }
    pendingRead += n;
    got += n;
    pcmCursor += n;
    if (pendingRead * 2 >= pending.size()) {
      pending.clear();
      pendingRead = 0;
    }
  }

  bool feedInput() {
    if (inputEos) {
      return true;
    }
    const ssize_t ix = AMediaCodec_dequeueInputBuffer(codec, 2000);
    if (ix < 0) {
      return true; // try outputs anyway
    }
    size_t bufSize = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec, (size_t)ix, &bufSize);
    if (!buf || bufSize == 0) {
      return false;
    }
    const ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, buf, bufSize);
    if (sampleSize < 0) {
      AMediaCodec_queueInputBuffer(codec, (size_t)ix, 0, 0, 0,
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      inputEos = true;
      return true;
    }
    const int64_t pts = AMediaExtractor_getSampleTime(extractor);
    AMediaCodec_queueInputBuffer(codec, (size_t)ix, 0, (size_t)sampleSize, pts, 0);
    AMediaExtractor_advance(extractor);
    return true;
  }

  void appendPcm(const uint8_t* data, size_t size) {
    if (!data || size == 0 || channels == 0) {
      return;
    }
    if (pendingRead > 0) {
      pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t)pendingRead * 2);
      pendingRead = 0;
    }
    const unsigned srcCh = channels;
    auto pushStereo = [&](float l, float r) {
      pending.push_back(l);
      pending.push_back(r);
    };

    if (pcmEncoding == kAudioFormatPcmFloat) {
      const size_t samples = size / sizeof(float);
      const float* f = reinterpret_cast<const float*>(data);
      for (size_t i = 0; i + srcCh <= samples; i += srcCh) {
        if (srcCh == 1) {
          pushStereo(f[i], f[i]);
        } else {
          pushStereo(f[i], f[i + 1]);
        }
      }
      return;
    }
    if (pcmEncoding == kAudioFormatPcm8bit) {
      const size_t samples = size;
      for (size_t i = 0; i + srcCh <= samples; i += srcCh) {
        auto u8 = [&](size_t idx) {
          return ((float)data[idx] - 128.0f) / 128.0f;
        };
        if (srcCh == 1) {
          const float s = u8(i);
          pushStereo(s, s);
        } else {
          pushStereo(u8(i), u8(i + 1));
        }
      }
      return;
    }
    if (pcmEncoding == kAudioFormatPcm32bit) {
      const size_t samples = size / sizeof(int32_t);
      const int32_t* p = reinterpret_cast<const int32_t*>(data);
      for (size_t i = 0; i + srcCh <= samples; i += srcCh) {
        auto s32 = [&](size_t idx) { return (float)p[idx] / 2147483648.0f; };
        if (srcCh == 1) {
          const float s = s32(i);
          pushStereo(s, s);
        } else {
          pushStereo(s32(i), s32(i + 1));
        }
      }
      return;
    }
    if (pcmEncoding == kAudioFormatPcm24bitPacked) {
      const size_t samples = size / 3;
      for (size_t i = 0; i + srcCh <= samples; i += srcCh) {
        auto s24 = [&](size_t idx) {
          const size_t o = idx * 3;
          int32_t v = (int32_t)data[o] | ((int32_t)data[o + 1] << 8) | ((int32_t)data[o + 2] << 16);
          if (v & 0x800000) {
            v |= ~0xFFFFFF;
          }
          return (float)v / 8388608.0f;
        };
        if (srcCh == 1) {
          const float s = s24(i);
          pushStereo(s, s);
        } else {
          pushStereo(s24(i), s24(i + 1));
        }
      }
      return;
    }

    // Default: 16-bit PCM.
    const size_t samples = size / sizeof(int16_t);
    const int16_t* p = reinterpret_cast<const int16_t*>(data);
    for (size_t i = 0; i + srcCh <= samples; i += srcCh) {
      auto s16 = [&](size_t idx) { return (float)p[idx] / 32768.0f; };
      if (srcCh == 1) {
        const float s = s16(i);
        pushStereo(s, s);
      } else {
        pushStereo(s16(i), s16(i + 1));
      }
    }
  }

  bool pump() {
    if (outputEos && pendingRead * 2 >= pending.size()) {
      return false;
    }
    if (!feedInput()) {
      return false;
    }

    AMediaCodecBufferInfo info{};
    const ssize_t ox = AMediaCodec_dequeueOutputBuffer(codec, &info, 2000);
    if (ox == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      return !outputEos || pendingRead * 2 < pending.size();
    }
    if (ox == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* outFmt = AMediaCodec_getOutputFormat(codec);
      if (outFmt) {
        int32_t ch = (int32_t)channels;
        int32_t sr = (int32_t)sampleRate;
        int32_t enc = pcmEncoding;
        AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
        AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
        if (AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_PCM_ENCODING, &enc)) {
          pcmEncoding = enc;
        }
        if (ch > 0) {
          channels = (unsigned)ch;
        }
        if (sr > 0) {
          sampleRate = (unsigned)sr;
        }
        AMediaFormat_delete(outFmt);
      }
      return true;
    }
    if (ox == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      return true;
    }
    if (ox < 0) {
      return false;
    }

    if (info.size > 0 && info.offset >= 0) {
      size_t outSize = 0;
      uint8_t* outBuf = AMediaCodec_getOutputBuffer(codec, (size_t)ox, &outSize);
      if (outBuf && (size_t)info.offset + (size_t)info.size <= outSize) {
        appendPcm(outBuf + info.offset, (size_t)info.size);
      }
    }
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      outputEos = true;
    }
    AMediaCodec_releaseOutputBuffer(codec, (size_t)ox, false);
    return true;
  }
};
