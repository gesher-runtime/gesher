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

}  // namespace gesher::media
