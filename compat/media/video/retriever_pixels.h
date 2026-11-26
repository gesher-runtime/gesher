// Target-sized MediaMetadataRetriever raster conversion. The decoded I420
// planes remain owned by the decoder; conversion retains at most two source
// rows and one ARGB row, in addition to the caller's final Bitmap storage.
// Images outside Skia's 16-bit sampling range also use one float output row.
#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>

class SkBitmap;

namespace gesher::media {
struct VideoFrame;

// Clockwise rotation from coded pixels to presentation orientation.
inline size_t RetrieverPixelIndex(int x, int y, int w, int h, int rotation) {
  switch (rotation) {
    case 90: return static_cast<size_t>(x) * h + (h - 1 - y);
    case 180: return static_cast<size_t>(h - 1 - y) * w + (w - 1 - x);
    case 270: return static_cast<size_t>(w - 1 - x) * h + y;
    default: return static_cast<size_t>(y) * w + x;
  }
}

// Keep the retriever's existing float/round aspect fit. Reject invalid metadata
// before a division or out-of-range float-to-int conversion. A dimension that
// rounds to zero is left as zero for Bitmap's IllegalArgumentException contract.
inline bool RetrieverTargetSize(int display_w, int display_h, int requested_w, int requested_h,
                                int* width, int* height) {
  if (display_w <= 0 || display_h <= 0) return false;
  if (requested_w <= 0 || requested_h <= 0) {
    *width = display_w;
    *height = display_h;
    return true;
  }
  const float scale = std::min(static_cast<float>(requested_w) / display_w,
                                static_cast<float>(requested_h) / display_h);
  const float w = std::round(display_w * scale), h = std::round(display_h * scale);
  if (!std::isfinite(w) || !std::isfinite(h) ||
      static_cast<double>(w) > std::numeric_limits<int>::max() ||
      static_cast<double>(h) > std::numeric_limits<int>::max()) return false;
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

struct RetrieverRasterStats {
  size_t scratch_bytes = 0;
  uint64_t converted_pixels = 0;
};

// Writes the actual final ARGB_8888 or RGB_565 bitmap. Scaling uses Skia's
// linear filter, with RGB_565 conversion before filtering just as the former
// createBitmap(colors, config) -> createScaledBitmap(..., true) sequence did.
// Returns false for invalid planes, unsupported storage or allocation failure.
bool RenderRetrieverBitmap(const VideoFrame& frame, int rotation, SkBitmap* output,
                           RetrieverRasterStats* stats = nullptr);
}  // namespace gesher::media
