// Gesher media: the encoders' selection and the bitstream helpers they share
// (encoder.h).

#include "encoder.h"

#include <cstdlib>
#include <cstring>

#include "../audio/log.h"

namespace gesher::media {

namespace {

// libavc's encoder (soft_avc_encoder.cc): H.264 where Media Foundation has none.
constexpr bool kSoftAvcEncoder = true;

bool SoftVideoOnly() {
  const char* v = getenv("GESHER_VIDEO_ENCODERS");
  return v != nullptr && strcmp(v, "soft") == 0;
}

}  // namespace

const std::vector<EncoderInfo>& AvailableEncoders() {
  static const std::vector<EncoderInfo> encoders = [] {
    std::vector<EncoderInfo> all;
    if (!SoftVideoOnly() && MfH264EncoderAvailable()) {
      all.push_back({"video/avc", "mf", true});
    } else if (kSoftAvcEncoder) {
      all.push_back({"video/avc", "soft", true});
    }
    if (MfAacEncoderAvailable()) all.push_back({"audio/mp4a-latm", "mf", false});
    all.push_back({"audio/opus", "soft", false});
    std::string list;
    for (const EncoderInfo& e : all) list += (list.empty() ? "" : ", ") + e.mime + " (" + e.backend + ")";
    MEDIA_LOGI("encoders: %s", list.c_str());
    return all;
  }();
  return encoders;
}

std::unique_ptr<VideoEncoder> VideoEncoder::Create(const std::string& mime, const VideoEncoderParams& p,
                                                   std::string* error) {
  if (mime != "video/avc") {
    *error = "no video encoder for " + mime;
    return nullptr;
  }
  if (p.width <= 0 || p.height <= 0 || (p.width & 1) || (p.height & 1)) {
    *error = "unsupported size " + std::to_string(p.width) + "x" + std::to_string(p.height);
    return nullptr;
  }
  for (const EncoderInfo& e : AvailableEncoders()) {
    if (e.mime != mime) continue;
    if (e.backend == "mf") {
      std::string mf_error;
      auto enc = CreateMfH264Encoder(p, &mf_error);
      if (enc != nullptr) return enc;
      MEDIA_LOGW("H.264 encoder: Media Foundation's: %s; libavc's instead", mf_error.c_str());
    }
    return CreateSoftAvcEncoder(p, error);
  }
  *error = "no video encoder for " + mime;
  return nullptr;
}

std::unique_ptr<AudioEncoder> AudioEncoder::Create(const std::string& mime, const AudioEncoderParams& p,
                                                   std::string* error) {
  if (p.channels < 1 || p.channels > 2) {
    *error = "unsupported channel count " + std::to_string(p.channels);
    return nullptr;
  }
  if (mime == "audio/opus") return CreateOpusEncoder(p, error);
  if (mime == "audio/mp4a-latm") {
    if (!MfAacEncoderAvailable()) {
      *error = "no AAC encoder on this machine (Media Foundation has none)";
      return nullptr;
    }
    return CreateMfAacEncoder(p, error);
  }
  *error = "no audio encoder for " + mime;
  return nullptr;
}

std::vector<std::pair<const uint8_t*, size_t>> SplitAnnexB(const uint8_t* p, size_t n) {
  std::vector<std::pair<const uint8_t*, size_t>> nals;
  size_t i = 0, start = SIZE_MAX;
  while (i + 3 <= n) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
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
