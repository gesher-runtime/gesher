// Execute the complete production row renderer against the previous full-image
// Bitmap pipeline using the same pinned Skia, without a decoder/JNI mock.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSamplingOptions.h"
#include "compat/media/video/retriever_pixels.h"
#include "compat/media/video/video_decoder.h"
#include "compat/media/video/yuv_rgba.h"

using namespace gesher::media;
namespace {
int checks = 0;
struct Comparison {
  uint64_t cases = 0, pixels = 0, different = 0;
  int largest_channel_step = 0;
} comparisons[2];

void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "retriever_pixels_test: %s\n", reason);
    std::exit(1);
  }
}

VideoFrame Frame(int w, int h, int padding = 0, int crop = 0, uint32_t seed = 1) {
  VideoFrame f;
  f.width = w + 2 * crop; f.height = h + 2 * crop;
  f.crop_left = f.crop_top = crop;
  f.crop_right = crop + w - 1; f.crop_bottom = crop + h - 1;
  f.stride_y = f.width + padding;
  f.stride_uv = (f.width + 1) / 2 + padding;
  f.data.resize(static_cast<size_t>(f.stride_y) * f.height +
                 2 * static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2));
  std::mt19937 random(seed);
  for (auto& v : f.data) v = static_cast<uint8_t>(random());
  return f;
}

SkBitmap Bitmap(int w, int h, bool rgb565) {
  SkBitmap result;
  Check(result.tryAllocPixels(SkImageInfo::Make(w, h,
            rgb565 ? kRGB_565_SkColorType : kN32_SkColorType,
            kPremul_SkAlphaType, SkColorSpace::MakeSRGB())), "allocate test bitmap");
  result.eraseColor(SK_ColorTRANSPARENT);
  return result;
}

// The pre-change pipeline: convert all visible I420 pixels, rotate the full
// ARGB array, write them into a source Bitmap of the requested config, then
// draw that entire Bitmap with the createScaledBitmap scale and linear filter.
SkBitmap FullImageReference(const VideoFrame& f, int rotation, int dw, int dh, bool rgb565) {
  const int w = f.visible_width(), h = f.visible_height();
  const bool turn = rotation == 90 || rotation == 270;
  const int sw = turn ? h : w, sh = turn ? w : h;
  std::vector<uint32_t> colors(static_cast<size_t>(sw) * sh);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int sx = x + f.crop_left, sy = y + f.crop_top;
      const uint32_t p = yuv::PixelToRgba(f.y()[sy * f.stride_y + sx],
          f.u()[(sy / 2) * f.stride_uv + sx / 2], f.v()[(sy / 2) * f.stride_uv + sx / 2]);
      size_t destination;
      switch (rotation) {
        case 90: destination = static_cast<size_t>(x) * h + h - 1 - y; break;
        case 180: destination = static_cast<size_t>(h - 1 - y) * w + w - 1 - x; break;
        case 270: destination = static_cast<size_t>(w - 1 - x) * h + y; break;
        default: destination = static_cast<size_t>(y) * w + x;
      }
      colors[destination] = 0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) |
                              ((p >> 16) & 0xffu);
    }
  }
  SkBitmap source = Bitmap(sw, sh, rgb565);
  const SkPixmap argb(SkImageInfo::Make(sw, sh, kBGRA_8888_SkColorType,
                        kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB()),
                        colors.data(), static_cast<size_t>(sw) * 4);
  Check(source.writePixels(argb), "reference initial Bitmap conversion");
  if (dw == sw && dh == sh) return source;
  SkBitmap output = Bitmap(dw, dh, rgb565);
  SkCanvas canvas(output);
  canvas.scale(static_cast<float>(dw) / sw, static_cast<float>(dh) / sh);
  const auto rect = SkRect::MakeIWH(sw, sh);
  canvas.drawImageRect(source.asImage(), rect, rect, SkSamplingOptions(SkFilterMode::kLinear),
                       nullptr, SkCanvas::kFast_SrcRectConstraint);
  return output;
}

void Compare(const VideoFrame& frame, int rotation, int dw, int dh, bool rgb565) {
  auto expected = FullImageReference(frame, rotation, dw, dh, rgb565);
  auto actual = Bitmap(dw, dh, rgb565);
  RetrieverRasterStats stats;
  Check(RenderRetrieverBitmap(frame, rotation, &actual, &stats), "production render succeeds");
  const bool turn = rotation == 90 || rotation == 270;
  const int sw = turn ? frame.visible_height() : frame.visible_width();
  const int sh = turn ? frame.visible_width() : frame.visible_height();
  const bool scaled = sw != dw || sh != dh;
  const size_t float_row = scaled && !rgb565 && (sw >= 32767 || sh >= 32767)
                            ? static_cast<size_t>(dw) * 16 : 0;
  Check(stats.scratch_bytes <= static_cast<size_t>(sw) * (scaled ? (rgb565 ? 8 : 12) : 4) + float_row,
        "scratch bound is three source rows plus a highp output row for oversized images");
  Check(stats.converted_pixels <= static_cast<uint64_t>(sw) * (scaled ? 2 * dh : sh),
        "only source rows reached by output samples are converted");
  if (scaled && dh >= sh) {
    Check(stats.converted_pixels <= static_cast<uint64_t>(sw) * sh,
          "adjacent/upscaled windows convert each source row at most once");
  }
  auto& summary = comparisons[rgb565];
  ++summary.cases;
  summary.pixels += static_cast<uint64_t>(dw) * dh;
  for (int y = 0; y < dh; ++y) {
    for (int x = 0; x < dw; ++x) {
      int difference = 0;
      if (rgb565) {
        const uint16_t a = *actual.getAddr16(x, y), b = *expected.getAddr16(x, y);
        difference = std::max({std::abs(int(a >> 11) - int(b >> 11)),
            std::abs(int((a >> 5) & 63) - int((b >> 5) & 63)),
            std::abs(int(a & 31) - int(b & 31))});
      } else {
        const auto a = actual.getColor(x, y), b = expected.getColor(x, y);
        Check(SkColorGetA(a) == 255, "all decoded pixels retain opaque alpha values");
        difference = std::max({std::abs(int(SkColorGetR(a)) - int(SkColorGetR(b))),
            std::abs(int(SkColorGetG(a)) - int(SkColorGetG(b))),
            std::abs(int(SkColorGetB(a)) - int(SkColorGetB(b)))});
      }
      summary.different += difference != 0;
      summary.largest_channel_step = std::max(summary.largest_channel_step, difference);
      if (difference > (scaled ? 1 : 0)) {
        std::fprintf(stderr, "case %dx%d -> %dx%d rotation=%d RGB565=%d pixel=%d,%d step=%d\n",
            frame.visible_width(), frame.visible_height(), dw, dh, rotation, rgb565, x, y, difference);
        Check(false, "filtered pixels exceed one output-format quantization step");
      }
    }
  }
}

void Cases() {
  const std::array<std::array<int, 4>, 16> sizes = {{{320, 240, 160, 120},
      {3840, 2160, 160, 90}, {319, 243, 161, 123}, {13, 17, 19, 29}, {10, 10, 17, 23},
      {1, 1, 9, 7}, {7, 1, 1, 11}, {1, 17, 11, 1}, {80, 90, 1, 1},
      {33, 63, 12, 32}, {257, 129, 113, 59}, {71, 83, 71, 83}, {3, 2, 3, 2}, {2, 3, 29, 31},
      {40001, 3, 199, 1}, {5, 40001, 1, 123}}};
  for (const auto& d : sizes) {
    auto frame = Frame(d[0], d[1], 7, 1);
    for (int rotation : {0, 90, 180, 270, 45}) {
      const bool turn = rotation == 90 || rotation == 270;
      for (bool rgb565 : {false, true}) {
        Compare(frame, rotation, d[2], d[3], rgb565);
        if (d[0] < 100 && d[1] < 100) {
          Compare(frame, rotation, turn ? d[1] : d[0], turn ? d[0] : d[1], rgb565);
        }
      }
    }
  }
  std::mt19937 random(0x17807);
  for (int n = 0; n < 256; ++n) {
    const int w = 1 + random() % 513, h = 1 + random() % 399;
    const int dw = 1 + random() % 277, dh = 1 + random() % 231;
    auto frame = Frame(w, h, n % 9, n % 4, random());
    for (bool rgb565 : {false, true}) Compare(frame, 90 * (n % 4), dw, dh, rgb565);
  }
}

void InvalidFrames() {
  auto output = Bitmap(5, 7, false);
  const auto valid = Frame(19, 21, 5, 1);
  for (int kind = 0; kind < 9; ++kind) {
    auto frame = valid;
    switch (kind) {
      case 0: frame.width = 0; break;
      case 1: frame.height = -1; break;
      case 2: frame.stride_y = frame.width - 1; break;
      case 3: frame.stride_uv = frame.width / 2; break;
      case 4: frame.crop_left = -1; break;
      case 5: frame.crop_right = frame.width; break;
      case 6: frame.crop_bottom = frame.crop_top - 1; break;
      case 7: frame.data.pop_back(); break;
      case 8: frame.height = std::numeric_limits<int>::max(); break;
    }
    output.eraseColor(SK_ColorMAGENTA);
    RetrieverRasterStats stats;
    Check(!RenderRetrieverBitmap(frame, 0, &output, &stats), "malformed frame is rejected");
    Check(output.getColor(0, 0) == SK_ColorMAGENTA && stats.converted_pixels == 0,
          "malformed input does not read or partially overwrite pixels");
  }
  Check(!RenderRetrieverBitmap(valid, 0, nullptr), "null output is rejected");
  SkBitmap empty;
  Check(!RenderRetrieverBitmap(valid, 0, &empty), "unallocated output is rejected");
}

void TargetDimensions() {
  int w = -1, h = -1;
  Check(RetrieverTargetSize(320, 240, 160, 160, &w, &h) && w == 160 && h == 120,
        "portrait bound retains wide display aspect ratio");
  Check(RetrieverTargetSize(240, 320, 160, 160, &w, &h) && w == 120 && h == 160,
        "rotated display aspect ratio swaps fitted dimensions");
  Check(RetrieverTargetSize(480, 240, -1, -1, &w, &h) && w == 480 && h == 240,
        "unscaled request keeps display/SAR dimensions");
  Check(RetrieverTargetSize(40001, 1, 1, 1, &w, &h) && w == 1 && h == 0,
        "zero-rounded dimension remains visible to the Bitmap argument error");
  Check(!RetrieverTargetSize(0, 240, 160, 160, &w, &h), "zero display width rejected");
  Check(!RetrieverTargetSize(320, -1, 160, 160, &w, &h), "negative display height rejected");
  Check(!RetrieverTargetSize(1, 1, std::numeric_limits<int>::max(),
                            std::numeric_limits<int>::max(), &w, &h),
        "float rounding past int range is rejected before conversion");
}

void BoundedRepeatedThumbnails() {
  const auto frame = Frame(3840, 2160);
  auto output = Bitmap(160, 90, false);
  for (int n = 0; n < 100; ++n) {
    RetrieverRasterStats stats;
    Check(RenderRetrieverBitmap(frame, 0, &output, &stats), "repeated 4K thumbnail render");
    Check(stats.scratch_bytes == 46080 && output.computeByteSize() == 57600,
          "4K thumbnail has 45KiB row scratch plus only its final 56.25KiB Bitmap");
    Check(stats.converted_pixels <= 691200, "4K thumbnail converts at most 180 source rows");
  }
}
}  // namespace

int main() {
  Cases();
  InvalidFrames();
  TargetDimensions();
  BoundedRepeatedThumbnails();
  for (int format = 0; format < 2; ++format) {
    const auto& s = comparisons[format];
    std::printf("%s: %llu cases, %llu pixels, %llu differing pixels, maximum channel step %d\n",
        format ? "RGB565" : "ARGB8888", static_cast<unsigned long long>(s.cases),
        static_cast<unsigned long long>(s.pixels), static_cast<unsigned long long>(s.different),
        s.largest_channel_step);
  }
  std::printf("retriever_pixels_test: %d checks; filtered crop/rotation, config and bounded row storage passed\n", checks);
  return 0;
}
