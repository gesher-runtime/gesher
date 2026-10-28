// Pure encoded-packet metadata; no encoder factory or Java dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace gesher::media {

// H.264 Annex B: splits a stream into its NAL units (start codes removed).
std::vector<std::pair<const uint8_t*, size_t>> SplitAnnexB(const uint8_t* p, size_t n);
// An AAC AudioSpecificConfig (2 bytes) for an object type, a rate and channels.
std::vector<uint8_t> AacAudioSpecificConfig(int object_type, uint32_t rate, uint32_t channels);
// An OpusHead (RFC 7845) for channels, a pre-skip and the input rate.
std::vector<uint8_t> OpusHead(uint32_t channels, uint32_t pre_skip, uint32_t rate);

}  // namespace gesher::media
