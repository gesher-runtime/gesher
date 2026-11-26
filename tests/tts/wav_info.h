// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// The format of a RIFF WAV byte array, for the Windows speech tests: what
// app/gesher-tts's WavPcm reads on the device, checked here against the actual
// stream Windows writes.
#pragma once

#include <stdint.h>
#include <string.h>

#include <vector>

struct WavInfo {
  bool ok = false;
  int format = 0;  // WAVE_FORMAT_PCM 1, IEEE float 3, extensible 0xFFFE
  int channels = 0;
  int sampleRate = 0;
  int bitsPerSample = 0;
  size_t dataOffset = 0;
  size_t dataBytes = 0;
};

inline uint32_t Le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline WavInfo ParseWav(const std::vector<uint8_t>& b) {
  WavInfo w;
  if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) != 0 || memcmp(b.data() + 8, "WAVE", 4) != 0) {
    return w;
  }
  bool fmt = false;
  size_t at = 12;
  while (at + 8 <= b.size()) {
    uint32_t size = Le32(&b[at + 4]);
    size_t body = at + 8;
    size_t available = b.size() - body;
    if (memcmp(&b[at], "fmt ", 4) == 0 && size >= 16 && available >= 16) {
      w.format = Le16(&b[body]);
      w.channels = Le16(&b[body + 2]);
      w.sampleRate = static_cast<int>(Le32(&b[body + 4]));
      w.bitsPerSample = Le16(&b[body + 14]);
      if (w.format == 0xFFFE && size >= 40 && available >= 26) w.format = Le16(&b[body + 24]);
      fmt = true;
    } else if (memcmp(&b[at], "data", 4) == 0) {
      w.dataOffset = body;
      w.dataBytes = size < available ? size : available;
      w.ok = fmt;
      return w;
    }
    if (size > available) break;
    at = body + size + (size & 1);
  }
  return w;
}

// The largest absolute 16-bit sample: 0 means digital silence.
inline int Peak16(const std::vector<uint8_t>& b, const WavInfo& w) {
  int peak = 0;
  for (size_t i = w.dataOffset; i + 1 < w.dataOffset + w.dataBytes; i += 2) {
    int v = static_cast<int16_t>(Le16(&b[i]));
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  return peak;
}

inline double Seconds(const WavInfo& w) {
  int frame = w.channels * (w.bitsPerSample / 8);
  return frame > 0 && w.sampleRate > 0
             ? static_cast<double>(w.dataBytes) / frame / w.sampleRate
             : 0.0;
}
