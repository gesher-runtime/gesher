// Gesher media: AOSP's software video decoders (video_decoder.h, ADR 0051):
// the codecs Media Foundation lacks on a machine. None are built yet: VP8/VP9
// (libvpx), AV1 (libdav1d) and the H.264/H.265 fallbacks (libavc, libhevc)
// come as each is ported (docs/media.md, stage 2).
#include "video_decoder.h"

namespace gesher::media {

std::vector<VideoCodecInfo> SoftVideoCodecs() { return {}; }

std::unique_ptr<VideoDecoder> CreateSoftVideoDecoder(const Format& track, std::string* error) {
  *error = "no software decoder for " + track.Mime();
  return nullptr;
}

}  // namespace gesher::media
