#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ID3v2 TBPM / TKEY (and v2.2 TBP / TKE). Album art is skipped; we only
// pull the two text frames DJ tools actually write.
namespace id3_meta {

struct Tags {
  float bpm = 0.0f;
  int key = -1; // 0..11 major, 12..23 minor; -1 unknown
};

inline unsigned int be24(const unsigned char* p) {
  return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | (unsigned int)p[2];
}

inline unsigned int be32(const unsigned char* p) {
  return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) |
         (unsigned int)p[3];
}

inline unsigned int synchsafe32(const unsigned char* p) {
  return ((unsigned int)(p[0] & 0x7f) << 21) | ((unsigned int)(p[1] & 0x7f) << 14) |
         ((unsigned int)(p[2] & 0x7f) << 7) | (unsigned int)(p[3] & 0x7f);
}

inline bool freadExact(FILE* f, void* dst, size_t n) {
  return std::fread(dst, 1, n, f) == n;
}

inline std::string decodeId3Text(const unsigned char* data, size_t n) {
  if (n == 0 || !data) {
    return {};
  }
  const unsigned char enc = data[0];
  const unsigned char* p = data + 1;
  size_t left = n - 1;
  std::string out;
  out.reserve(left);
  if (enc == 0 || enc == 3) { // ISO-8859-1 or UTF-8; BPM/key are ASCII
    for (size_t i = 0; i < left; ++i) {
      if (p[i] == 0) {
        break;
      }
      out.push_back((char)p[i]);
    }
    return out;
  }
  // UTF-16 with BOM (1) or UTF-16BE (2). BPM/key stay in ASCII.
  size_t i = 0;
  bool le = enc == 1;
  if (enc == 1 && left >= 2) {
    if (p[0] == 0xff && p[1] == 0xfe) {
      le = true;
      i = 2;
    } else if (p[0] == 0xfe && p[1] == 0xff) {
      le = false;
      i = 2;
    }
  }
  for (; i + 1 < left; i += 2) {
    const unsigned int cp = le ? (unsigned int)p[i] | ((unsigned int)p[i + 1] << 8)
                               : ((unsigned int)p[i] << 8) | (unsigned int)p[i + 1];
    if (cp == 0) {
      break;
    }
    if (cp < 128) {
      out.push_back((char)cp);
    }
  }
  return out;
}

inline float parseBpmString(const std::string& raw) {
  const char* s = raw.c_str();
  while (*s && std::isspace((unsigned char)*s)) {
    ++s;
  }
  char* end = nullptr;
  const float v = std::strtof(s, &end);
  if (end == s || !(v > 20.0f) || v > 400.0f) {
    return 0.0f;
  }
  return v;
}

inline int noteIndex(const std::string& n) {
  static const char* kNat[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  static const char* kFlat[] = {"C", "DB", "D", "EB", "E", "F", "GB", "G", "AB", "A", "BB", "B"};
  for (int i = 0; i < 12; ++i) {
    if (n == kNat[i] || n == kFlat[i]) {
      return i;
    }
  }
  if (n == "CB") {
    return 11;
  }
  if (n == "E#") {
    return 5;
  }
  if (n == "B#") {
    return 0;
  }
  return -1;
}

// Camelot majors 0=C .. 11=B, same tables as Dart music_utils.dart.
inline int parseKeyString(std::string raw) {
  while (!raw.empty() && std::isspace((unsigned char)raw.front())) {
    raw.erase(raw.begin());
  }
  while (!raw.empty() && std::isspace((unsigned char)raw.back())) {
    raw.pop_back();
  }
  if (raw.empty()) {
    return -1;
  }
  for (char& c : raw) {
    if (c == 'a' || (c >= 'b' && c <= 'z')) {
      c = (char)(c - 'a' + 'A');
    }
  }
  std::string compact;
  compact.reserve(raw.size());
  for (char c : raw) {
    if (!std::isspace((unsigned char)c) && c != '-') {
      compact.push_back(c);
    }
  }

  // Camelot 1A–12A / 1B–12B, or Open Key 1m/1d.
  if (compact.size() >= 2 && compact.size() <= 3) {
    int num = 0;
    size_t i = 0;
    while (i < compact.size() && std::isdigit((unsigned char)compact[i])) {
      num = num * 10 + (compact[i] - '0');
      ++i;
    }
    if (num >= 1 && num <= 12 && i == compact.size() - 1) {
      const char letter = compact[i];
      static const int kCamelotMajorRoot[12] = {
          /*1B*/ 11, /*2B*/ 6, /*3B*/ 1, /*4B*/ 8, /*5B*/ 3, /*6B*/ 10,
          /*7B*/ 5,  /*8B*/ 0, /*9B*/ 7, /*10B*/ 2, /*11B*/ 9, /*12B*/ 4,
      };
      const int root = kCamelotMajorRoot[num - 1];
      if (letter == 'B') {
        return root;
      }
      if (letter == 'A') {
        return ((root - 3 + 12) % 12) + 12;
      }
    }
  }

  bool minor = false;
  std::string note = compact;
  if (note.size() > 5 && note.compare(note.size() - 5, 5, "MINOR") == 0) {
    minor = true;
    note.resize(note.size() - 5);
  } else if (note.size() > 3 && note.compare(note.size() - 3, 3, "MIN") == 0) {
    minor = true;
    note.resize(note.size() - 3);
  } else if (note.size() > 5 && note.compare(note.size() - 5, 5, "MAJOR") == 0) {
    note.resize(note.size() - 5);
  } else if (note.size() > 3 && note.compare(note.size() - 3, 3, "MAJ") == 0) {
    note.resize(note.size() - 3);
  } else if (!note.empty() && note.back() == 'M') {
    minor = true;
    note.pop_back();
  }
  const int pc = noteIndex(note);
  if (pc < 0) {
    return -1;
  }
  return minor ? pc + 12 : pc;
}

inline void applyTextFrame(Tags& out, const char* id, const std::string& text) {
  if (std::strcmp(id, "TBPM") == 0 || std::strcmp(id, "TBP") == 0) {
    const float bpm = parseBpmString(text);
    if (bpm > 1.0f) {
      out.bpm = bpm;
    }
  } else if (std::strcmp(id, "TKEY") == 0 || std::strcmp(id, "TKE") == 0) {
    const int key = parseKeyString(text);
    if (key >= 0) {
      out.key = key;
    }
  }
}

inline Tags parseId3v2(FILE* f) {
  Tags out;
  unsigned char hdr[10];
  if (!freadExact(f, hdr, 10) || std::memcmp(hdr, "ID3", 3) != 0) {
    return out;
  }
  const int ver = hdr[3];
  if (ver < 2 || ver > 4) {
    return out;
  }
  const unsigned int flags = hdr[5];
  unsigned int tagSize = synchsafe32(hdr + 6);
  if (tagSize < 10 || tagSize > 16u * 1024u * 1024u) {
    return out;
  }
  if (ver >= 3 && (flags & 0x40)) { // skip extended header
    unsigned char sz[4];
    if (!freadExact(f, sz, 4)) {
      return out;
    }
    const unsigned int ext = ver == 4 ? synchsafe32(sz) : be32(sz);
    if (ext < 4 || ext > tagSize) {
      return out;
    }
    if (std::fseek(f, (long)ext - 4, SEEK_CUR) != 0) {
      return out;
    }
    tagSize -= ext;
  }

  unsigned int consumed = 0;
  while (consumed + 6 < tagSize) {
    if (ver == 2) {
      unsigned char fh[6];
      if (!freadExact(f, fh, 6)) {
        break;
      }
      consumed += 6;
      if (fh[0] == 0) {
        break;
      }
      char id[4] = {(char)fh[0], (char)fh[1], (char)fh[2], 0};
      const unsigned int size = be24(fh + 3);
      if (size > tagSize - consumed) {
        break;
      }
      if (std::strcmp(id, "TBP") == 0 || std::strcmp(id, "TKE") == 0) {
        std::vector<unsigned char> body(size);
        if (size > 0 && !freadExact(f, body.data(), size)) {
          break;
        }
        applyTextFrame(out, id, decodeId3Text(body.data(), body.size()));
      } else if (std::fseek(f, (long)size, SEEK_CUR) != 0) {
        break;
      }
      consumed += size;
    } else {
      unsigned char fh[10];
      if (!freadExact(f, fh, 10)) {
        break;
      }
      consumed += 10;
      if (fh[0] == 0) {
        break;
      }
      char id[5] = {(char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0};
      const unsigned int size = ver == 4 ? synchsafe32(fh + 4) : be32(fh + 4);
      if (size > tagSize - consumed) {
        break;
      }
      if (std::strcmp(id, "TBPM") == 0 || std::strcmp(id, "TKEY") == 0) {
        std::vector<unsigned char> body(size);
        if (size > 0 && !freadExact(f, body.data(), size)) {
          break;
        }
        applyTextFrame(out, id, decodeId3Text(body.data(), body.size()));
      } else if (std::fseek(f, (long)size, SEEK_CUR) != 0) {
        break;
      }
      consumed += size;
    }
    if (out.bpm > 1.0f && out.key >= 0) {
      break;
    }
  }
  return out;
}

inline Tags read(const char* path) {
  Tags out;
  if (!path) {
    return out;
  }
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return out;
  }
  out = parseId3v2(f);
  std::fclose(f);
  return out;
}

} // namespace id3_meta
