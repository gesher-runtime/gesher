// Pure codec configuration helpers shared by encoders and native muxers.
#include "codec_config.h"

#include <cstring>

namespace gesher::media {

std::vector<std::pair<const uint8_t*, size_t>> SplitAnnexB(const uint8_t* p, size_t n) {
  std::vector<std::pair<const uint8_t*, size_t>> nals;
  size_t i = 0, start = SIZE_MAX;
  while (n - i >= 3) {
    if (p[i] == 0) {
      if (p[i + 1] == 0 && p[i + 2] == 1) {
        if (start != SIZE_MAX) {
          size_t end = i;
          while (end > start && p[end - 1] == 0) end--;  // a 4-byte start code's zero
          nals.emplace_back(p + start, end - start);
        }
        i += 3;
        start = i;
      } else {
        i++;
      }
      continue;
    }
    // Every possible start code begins with a zero. Skip a complete word only
    // when none of its bytes can start one; use the original scalar checks
    // above for zero bytes. Only a nonzero byte can enter this probe. The mask
    // is a boolean, never a position map (subtraction can borrow across bytes).
    if (n - i >= sizeof(uint64_t)) {
      uint64_t word;
      std::memcpy(&word, p + i, sizeof(word));
      constexpr uint64_t low = 0x0101010101010101ULL;
      constexpr uint64_t high = 0x8080808080808080ULL;
      if (((word - low) & ~word & high) == 0) {
        i += sizeof(word);
        continue;
      }
    }
    i++;
  }
  if (start != SIZE_MAX && start < n) nals.emplace_back(p + start, n - start);
  return nals;
}

std::vector<uint8_t> AacAudioSpecificConfig(int object_type, uint32_t rate, uint32_t channels) {
  static const uint32_t kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                    22050, 16000, 12000, 11025, 8000,  7350};
  int index = 4;
  for (int i = 0; i < 13; i++) {
    if (kRates[i] == rate) index = i;
  }
  const uint16_t v = static_cast<uint16_t>((object_type << 11) | (index << 7) | (channels << 3));
  return {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xff)};
}

std::vector<uint8_t> OpusHead(uint32_t channels, uint32_t pre_skip, uint32_t rate) {
  std::vector<uint8_t> h = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<uint8_t>(channels)};
  h.push_back(static_cast<uint8_t>(pre_skip & 0xff));
  h.push_back(static_cast<uint8_t>(pre_skip >> 8));
  for (int i = 0; i < 4; i++) h.push_back(static_cast<uint8_t>(rate >> (8 * i)));
  h.push_back(0);  // output gain
  h.push_back(0);
  h.push_back(0);  // channel mapping family 0 (mono, stereo)
  return h;
}

}  // namespace gesher::media
