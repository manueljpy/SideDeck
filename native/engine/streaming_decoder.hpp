#pragma once

// Streaming decoder for local files. Include after dr_mp3 / dr_wav / dr_flac
// and (for Opus) opusfile.h. Keep the decoder open, seek, read chunks.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct StreamingDecoder {
  enum class Kind { None, Wav, Mp3, Flac, Opus };

  Kind kind = Kind::None;
  drwav wav{};
  drmp3 mp3{};
  drflac* flac = nullptr;
  OggOpusFile* opus = nullptr;
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
    } else if (kind == Kind::Flac && flac) {
      drflac_close(flac);
    } else if (kind == Kind::Opus && opus) {
      op_free(opus);
    }
    flac = nullptr;
    opus = nullptr;
    kind = Kind::None;
    channels = 0;
    sampleRate = 0;
    totalFrames = 0;
    seekPoints.clear();
    std::memset(&wav, 0, sizeof(wav));
    std::memset(&mp3, 0, sizeof(mp3));
  }

  static bool extIs(const char* path, const char* ext) {
    const size_t elen = std::strlen(ext);
    const size_t plen = std::strlen(path);
    if (plen < elen || elen < 1) {
      return false;
    }
    for (size_t i = 0; i < elen; ++i) {
      const unsigned char a = (unsigned char)path[plen - elen + i];
      const unsigned char b = (unsigned char)ext[i];
      if (std::tolower(a) != std::tolower(b)) {
        return false;
      }
    }
    return true;
  }

  bool open(const char* path) {
    close();
    if (!path || !path[0]) {
      return false;
    }

    // Enough of the first Ogg page to see OpusHead / vorbis after the page hdr.
    unsigned char head[64]{};
    const size_t nHead = readHead(path, head, sizeof(head));

    // Definite containers: try only the matching decoder. Falling through to
    // MP3 on an Ogg Vorbis file makes dr_mp3 scan for frame sync forever
    // (analyze-all looks hung on the first .ogg).
    if (isOgg(head, nHead) || extIs(path, ".opus") || extIs(path, ".ogg")) {
      return tryOpus(path);
    }
    if (isFlac(head, nHead) || extIs(path, ".flac")) {
      if (tryFlac(path)) {
        return true;
      }
      // Mislabeled .flac still gets a fallthrough below.
    } else if (isWav(head, nHead) || extIs(path, ".wav")) {
      if (tryWav(path)) {
        return true;
      }
    } else if (extIs(path, ".mp3")) {
      if (tryMp3(path)) {
        return true;
      }
    }

    static constexpr Kind kOrder[] = {Kind::Wav, Kind::Flac, Kind::Mp3};
    for (Kind k : kOrder) {
      if (tryKind(k, path)) {
        return true;
      }
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
    if (kind == Kind::Flac && flac) {
      return flac->currentPCMFrame;
    }
    if (kind == Kind::Opus && opus) {
      const ogg_int64_t pos = op_pcm_tell(opus);
      return pos < 0 ? 0 : (uint64_t)pos;
    }
    return 0;
  }

  // A bound table makes dr_mp3 handle the bit reservoir on seek itself.
  bool hasSeekTable() const { return kind == Kind::Mp3 && !seekPoints.empty(); }

  bool seek(uint64_t frame) {
    if (kind == Kind::Wav) {
      return drwav_seek_to_pcm_frame(&wav, frame) == DRWAV_TRUE;
    }
    if (kind == Kind::Mp3) {
      return drmp3_seek_to_pcm_frame(&mp3, frame) == DRMP3_TRUE;
    }
    if (kind == Kind::Flac && flac) {
      return drflac_seek_to_pcm_frame(flac, frame) == DRFLAC_TRUE;
    }
    if (kind == Kind::Opus && opus) {
      return op_pcm_seek(opus, (ogg_int64_t)frame) == 0;
    }
    return false;
  }

  // Always writes interleaved stereo. Returns frames actually read.
  // May take several underlying codec calls to fill `frames`: opusfile in
  // particular returns one packet at a time (~2.5–60 ms), so a single call
  // cannot fill an 8192-frame chunk.
  uint64_t readStereo(float* out, uint64_t frames) {
    if (!out || frames == 0 || kind == Kind::None || channels == 0) {
      return 0;
    }
    uint64_t got = 0;
    while (got < frames) {
      const uint64_t n = readStereoOnce(out + got * 2, frames - got);
      if (n == 0) {
        break;
      }
      got += n;
    }
    return got;
  }

 private:
  // One shot at the underlying codec. Opus may return far fewer than requested.
  uint64_t readStereoOnce(float* out, uint64_t frames) {
    if (kind == Kind::Opus && opus) {
      if (channels >= 2) {
        const int n = op_read_float_stereo(opus, out, (int)frames * 2);
        return n < 0 ? 0 : (uint64_t)n;
      }
      std::vector<float> tmp((size_t)frames);
      const int n = op_read_float(opus, tmp.data(), (int)frames, nullptr);
      if (n <= 0) {
        return 0;
      }
      for (int i = 0; i < n; ++i) {
        out[i * 2] = out[i * 2 + 1] = tmp[(size_t)i];
      }
      return (uint64_t)n;
    }
    if (channels == 2) {
      return readInterleaved(out, frames);
    }
    std::vector<float> tmp((size_t)frames * channels);
    const uint64_t n = readInterleaved(tmp.data(), frames);
    for (uint64_t i = 0; i < n; ++i) {
      if (channels == 1) {
        out[i * 2] = out[i * 2 + 1] = tmp[(size_t)i];
      } else {
        out[i * 2] = tmp[(size_t)i * channels];
        out[i * 2 + 1] = tmp[(size_t)i * channels + 1];
      }
    }
    return n;
  }

  static size_t readHead(const char* path, unsigned char* out, size_t n) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
      return 0;
    }
    const size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got;
  }

  static bool isWav(const unsigned char* b, size_t n) {
    return n >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' &&
           b[9] == 'A' && b[10] == 'V' && b[11] == 'E';
  }

  static bool isFlac(const unsigned char* b, size_t n) {
    return n >= 4 && b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C';
  }

  static bool isOgg(const unsigned char* b, size_t n) {
    return n >= 4 && b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S';
  }

  bool tryKind(Kind k, const char* path) {
    switch (k) {
      case Kind::Wav:
        return tryWav(path);
      case Kind::Flac:
        return tryFlac(path);
      case Kind::Opus:
        return tryOpus(path);
      case Kind::Mp3:
        return tryMp3(path);
      case Kind::None:
        return false;
    }
    return false;
  }

  uint64_t readInterleaved(float* out, uint64_t frames) {
    if (kind == Kind::Wav) {
      return drwav_read_pcm_frames_f32(&wav, frames, out);
    }
    if (kind == Kind::Flac && flac) {
      return drflac_read_pcm_frames_f32(flac, frames, out);
    }
    if (kind == Kind::Mp3) {
      return drmp3_read_pcm_frames_f32(&mp3, frames, out);
    }
    return 0;
  }

  bool acceptOpen(unsigned ch, unsigned sr, uint64_t frames) {
    if (ch == 0 || sr == 0 || frames == 0) {
      close();
      return false;
    }
    channels = ch;
    sampleRate = sr;
    totalFrames = frames;
    return true;
  }

  bool tryWav(const char* path) {
    if (!drwav_init_file(&wav, path, nullptr)) {
      return false;
    }
    kind = Kind::Wav;
    return acceptOpen(wav.channels, wav.sampleRate, wav.totalPCMFrameCount);
  }

  bool tryFlac(const char* path) {
    flac = drflac_open_file(path, nullptr);
    if (!flac) {
      return false;
    }
    kind = Kind::Flac;
    return acceptOpen(flac->channels, flac->sampleRate, flac->totalPCMFrameCount);
  }

  bool tryOpus(const char* path) {
    int err = 0;
    opus = op_open_file(path, &err);
    if (!opus || err != 0) {
      opus = nullptr;
      return false;
    }
    if (!op_seekable(opus)) {
      op_free(opus);
      opus = nullptr;
      return false;
    }
    const ogg_int64_t total = op_pcm_total(opus, -1);
    const int ch = op_channel_count(opus, -1);
    if (total <= 0 || ch < 1) {
      op_free(opus);
      opus = nullptr;
      return false;
    }
    kind = Kind::Opus;
    // Decoded Opus is always 48 kHz.
    return acceptOpen((unsigned)ch, 48000, (uint64_t)total);
  }

  bool tryMp3(const char* path) {
    if (!drmp3_init_file(&mp3, path, nullptr)) {
      return false;
    }
    kind = Kind::Mp3;
    if (!acceptOpen(mp3.channels, mp3.sampleRate, drmp3_get_pcm_frame_count(&mp3))) {
      return false;
    }
    // Seek table so a backward jump does not restart the file. Without this,
    // dr_mp3 brute-force seek goes to byte 0 whenever the target is behind
    // the cursor. Density matters as much as presence: between points dr_mp3
    // has to decode forward to reach the target, and that is dead time on a
    // cue jump. Roughly 10 per second lands within 100 ms of anywhere, at
    // 16 bytes per point.
    const double seconds = (double)totalFrames / (double)sampleRate;
    drmp3_uint32 n = (drmp3_uint32)std::min(std::max(seconds * 10.0, 512.0), 16384.0);
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
