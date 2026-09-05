#pragma once

// Streaming MP3/WAV decoder. Include after dr_mp3.h / dr_wav.h.
// Keep the decoder open, seek, read chunks.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct StreamingDecoder {
  enum class Kind { None, Wav, Mp3 };

  Kind kind = Kind::None;
  drwav wav{};
  drmp3 mp3{};
  unsigned channels = 0;
  unsigned sampleRate = 0;
  uint64_t totalFrames = 0;
  std::vector<drmp3_seek_point> seekPoints;

  StreamingDecoder() = default;
  StreamingDecoder(const StreamingDecoder&) = delete;
  StreamingDecoder& operator=(const StreamingDecoder&) = delete;
  ~StreamingDecoder() { close(); }

  void close() {
    if (kind == Kind::Wav) {
      drwav_uninit(&wav);
    } else if (kind == Kind::Mp3) {
      drmp3_bind_seek_table(&mp3, 0, nullptr);
      drmp3_uninit(&mp3);
    }
    kind = Kind::None;
    channels = 0;
    sampleRate = 0;
    totalFrames = 0;
    seekPoints.clear();
    std::memset(&wav, 0, sizeof(wav));
    std::memset(&mp3, 0, sizeof(mp3));
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

  static bool extIs(const char* path, const char* ext) {
    const std::string p(path);
    if (p.size() < 4) {
      return false;
    }
    std::string e = p.substr(p.size() - 4);
    for (char& c : e) {
      c = (char)std::tolower((unsigned char)c);
    }
    return e == ext;
  }

  bool open(const char* path) {
    close();
    if (!path || !path[0]) {
      return false;
    }
    const bool preferWav = extIs(path, ".wav") || fileLooksLikeWav(path);
    if (preferWav && tryWav(path)) {
      return true;
    }
    if (tryMp3(path)) {
      return true;
    }
    if (!preferWav && tryWav(path)) {
      return true;
    }
    return false;
  }

  uint64_t tell() const {
    if (kind == Kind::Wav) {
      return wav.readCursorInPCMFrames;
    }
    if (kind == Kind::Mp3) {
      return mp3.currentPCMFrame;
    }
    return 0;
  }

  bool seek(uint64_t frame) {
    if (kind == Kind::Wav) {
      return drwav_seek_to_pcm_frame(&wav, frame) == DRWAV_TRUE;
    }
    if (kind == Kind::Mp3) {
      return drmp3_seek_to_pcm_frame(&mp3, frame) == DRMP3_TRUE;
    }
    return false;
  }

  // Always writes interleaved stereo. Returns frames actually read.
  uint64_t readStereo(float* out, uint64_t frames) {
    if (!out || frames == 0 || kind == Kind::None || channels == 0) {
      return 0;
    }
    if (channels == 2) {
      if (kind == Kind::Wav) {
        return drwav_read_pcm_frames_f32(&wav, frames, out);
      }
      return drmp3_read_pcm_frames_f32(&mp3, frames, out);
    }
    std::vector<float> tmp((size_t)frames * channels);
    uint64_t got = 0;
    if (kind == Kind::Wav) {
      got = drwav_read_pcm_frames_f32(&wav, frames, tmp.data());
    } else {
      got = drmp3_read_pcm_frames_f32(&mp3, frames, tmp.data());
    }
    for (uint64_t i = 0; i < got; ++i) {
      if (channels == 1) {
        out[i * 2] = out[i * 2 + 1] = tmp[(size_t)i];
      } else {
        out[i * 2] = tmp[(size_t)i * channels];
        out[i * 2 + 1] = tmp[(size_t)i * channels + 1];
      }
    }
    return got;
  }

  int64_t engineFrameCount(int engineSr) const {
    if (sampleRate <= 0 || engineSr <= 0) {
      return 0;
    }
    return (int64_t)((double)totalFrames * (double)engineSr / (double)sampleRate);
  }

 private:
  bool tryWav(const char* path) {
    if (!drwav_init_file(&wav, path, nullptr)) {
      return false;
    }
    kind = Kind::Wav;
    channels = wav.channels;
    sampleRate = wav.sampleRate;
    totalFrames = wav.totalPCMFrameCount;
    if (channels == 0 || sampleRate == 0 || totalFrames == 0) {
      close();
      return false;
    }
    return true;
  }

  bool tryMp3(const char* path) {
    if (!drmp3_init_file(&mp3, path, nullptr)) {
      return false;
    }
    kind = Kind::Mp3;
    channels = mp3.channels;
    sampleRate = mp3.sampleRate;
    totalFrames = drmp3_get_pcm_frame_count(&mp3);
    if (channels == 0 || sampleRate == 0 || totalFrames == 0) {
      close();
      return false;
    }
    // Seek table so a backward jump does not restart the file. Without this,
    // dr_mp3 brute-force seek goes to byte 0 whenever the target is behind
    // the cursor.
    drmp3_uint32 n = 512;
    seekPoints.assign(n, drmp3_seek_point{});
    if (drmp3_calculate_seek_points(&mp3, &n, seekPoints.data()) == DRMP3_TRUE && n > 0) {
      seekPoints.resize(n);
      drmp3_bind_seek_table(&mp3, n, seekPoints.data());
    } else {
      seekPoints.clear();
    }
    return true;
  }
};

inline void scanWaveform(StreamingDecoder& dec, int bins, std::vector<float>& waveMin,
                         std::vector<float>& waveMax) {
  waveMin.assign((size_t)std::max(1, bins), 0.0f);
  waveMax.assign((size_t)std::max(1, bins), 0.0f);
  if (dec.totalFrames == 0 || bins < 1) {
    return;
  }
  dec.seek(0);
  constexpr uint64_t kBuf = 4096;
  std::vector<float> buf((size_t)kBuf * 2);
  uint64_t pos = 0;
  while (pos < dec.totalFrames) {
    const uint64_t got = dec.readStereo(buf.data(), kBuf);
    if (got == 0) {
      break;
    }
    for (uint64_t i = 0; i < got; ++i) {
      const int b = (int)std::min((int64_t)bins - 1,
                                    (int64_t)((pos + i) * (uint64_t)bins / dec.totalFrames));
      const float s = 0.5f * (buf[(size_t)i * 2] + buf[(size_t)i * 2 + 1]);
      waveMin[(size_t)b] = std::min(waveMin[(size_t)b], s);
      waveMax[(size_t)b] = std::max(waveMax[(size_t)b], s);
    }
    pos += got;
  }
}
