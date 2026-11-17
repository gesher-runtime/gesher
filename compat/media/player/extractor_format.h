// Normalize plugin track formats for the framework and NDK MediaExtractor APIs.
#pragma once

#include <media/NdkMediaFormat.h>

#include "../video/video_decoder.h"
#include "extractor.h"

namespace gesher::media {

// A track's format as NuMediaExtractor gives it: H.264's avcC as csd-0 (the
// SPSs) and csd-1 (the PPSs), H.265's hvcC as csd-0, with start codes.
inline Format ExtractorTrackFormat(Extractor* ex, size_t index) {
  Format f = ex->TrackFormat(index);
  std::string mime = f.Mime();
  std::vector<uint8_t> b, have;
  if (mime == "video/avc" && f.GetBuffer("csd-avc", &b) && !f.GetBuffer("csd-0", &have)) {
    std::vector<uint8_t> sps, pps;
    if (AvccToAnnexB(b.data(), b.size(), &sps, &pps)) {
      AMediaFormat_setBuffer(f.get(), "csd-0", sps.data(), sps.size());
      if (!pps.empty()) AMediaFormat_setBuffer(f.get(), "csd-1", pps.data(), pps.size());
    }
  } else if (mime == "video/hevc" && f.GetBuffer("csd-hevc", &b) && !f.GetBuffer("csd-0", &have)) {
    std::vector<uint8_t> ps;
    if (HvccToAnnexB(b.data(), b.size(), &ps)) AMediaFormat_setBuffer(f.get(), "csd-0", ps.data(), ps.size());
  } else if (mime == "video/mp4v-es") {
    AddCsdFromEsds(f);  // MPEG-4 part 2's VOL header
  }
  return f;
}

}  // namespace gesher::media
