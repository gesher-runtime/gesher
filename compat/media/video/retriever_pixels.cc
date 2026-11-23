#include "retriever_pixels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSamplingOptions.h"
#include "video_decoder.h"
#include "yuv_rgba.h"

namespace gesher::media {
namespace {
bool ValidFrame(const VideoFrame& f) {
  if (f.width <= 0 || f.height <= 0 || f.stride_y < f.width ||
      f.stride_uv < f.width / 2 + f.width % 2 || f.crop_left < 0 || f.crop_top < 0 ||
      f.crop_right < f.crop_left || f.crop_bottom < f.crop_top ||
      f.crop_right >= f.width || f.crop_bottom >= f.height) return false;
  const uint64_t y_bytes = static_cast<uint64_t>(f.stride_y) * f.height;
  const uint64_t uv_bytes = static_cast<uint64_t>(f.stride_uv) *
                            (f.height / 2 + f.height % 2);
  return y_bytes + 2 * uv_bytes <= f.data.size();
}

uint32_t RgbaToArgb(uint32_t rgba) {
  return 0xff000000u | ((rgba & 0xffu) << 16) | (rgba & 0xff00u) |
         ((rgba >> 16) & 0xffu);
}

void ConvertRow(const VideoFrame& frame, int rotation, int row, int width, int height,
                uint32_t* argb) {
  const int coded_width = frame.visible_width();
  const int inverse = rotation == 90 ? 270 : rotation == 270 ? 90 : rotation;
  const uint8_t* y_plane = frame.data.data();
  const uint8_t* u_plane = y_plane + static_cast<size_t>(frame.stride_y) * frame.height;
  const uint8_t* v_plane = u_plane + static_cast<size_t>(frame.stride_uv) *
                                    (frame.height / 2 + frame.height % 2);
  if (rotation != 90 && rotation != 270) {
    // Keep the decoder's vectorized row conversion for contiguous source rows.
    // An odd crop starts in the second half of a chroma pair: convert that one
    // pixel first, then pass the aligned span to the same SIMD helper. Reorder
    // channels and a 180-degree presentation in this one existing row buffer.
    const int sy = frame.crop_top + (rotation == 180 ? height - 1 - row : row);
    const uint8_t* y = y_plane + static_cast<size_t>(sy) * frame.stride_y + frame.crop_left;
    const uint8_t* u = u_plane + static_cast<size_t>(sy / 2) * frame.stride_uv + frame.crop_left / 2;
    const uint8_t* v = v_plane + static_cast<size_t>(sy / 2) * frame.stride_uv + frame.crop_left / 2;
    const int first = frame.crop_left & 1;
    if (first != 0) argb[0] = yuv::PixelToRgba(y[0], u[0], v[0]);
    if (first < width) {
      yuv::RowToRgba(y + first, u + first, v + first, argb + first, width - first);
    }
    if (rotation == 180) {
      for (int left = 0, right = width - 1; left <= right; ++left, --right) {
        const uint32_t a = RgbaToArgb(argb[left]), b = RgbaToArgb(argb[right]);
        argb[left] = b;
        argb[right] = a;
      }
    } else {
      for (int x = 0; x < width; ++x) argb[x] = RgbaToArgb(argb[x]);
    }
    return;
  }
  for (int x = 0; x < width; ++x) {
    const size_t source = RetrieverPixelIndex(x, row, width, height, inverse);
    const size_t sx = frame.crop_left + source % coded_width;
    const size_t sy = frame.crop_top + source / coded_width;
    const size_t uv = (sy / 2) * frame.stride_uv + sx / 2;
    const uint32_t rgba = yuv::PixelToRgba(y_plane[sy * frame.stride_y + sx],
                                         u_plane[uv], v_plane[uv]);
    argb[x] = RgbaToArgb(rgba);
  }
}
}  // namespace

bool RenderRetrieverBitmap(const VideoFrame& frame, int rotation, SkBitmap* output,
                           RetrieverRasterStats* stats) {
  if (stats != nullptr) *stats = {};
  if (!ValidFrame(frame) || output == nullptr || output->width() <= 0 ||
      output->height() <= 0 || output->getPixels() == nullptr ||
      (output->colorType() != kN32_SkColorType && output->colorType() != kRGB_565_SkColorType)) {
    return false;
  }
  int width = frame.visible_width(), height = frame.visible_height();
  if (rotation == 90 || rotation == 270) std::swap(width, height);
  const bool scaled = width != output->width() || height != output->height();
  std::unique_ptr<uint32_t[]> argb(new (std::nothrow) uint32_t[width]);
  if (argb == nullptr) return false;
  const auto argb_info = SkImageInfo::Make(width, 1, kBGRA_8888_SkColorType,
                                          kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
  const SkPixmap argb_row(argb_info, argb.get(), static_cast<size_t>(width) * 4);
  if (stats != nullptr) stats->scratch_bytes = argb_row.rowBytes();
  if (!scaled) {
    for (int y = 0; y < height; ++y) {
      ConvertRow(frame, rotation, y, width, height, argb.get());
      if (!output->writePixels(argb_row, 0, y)) return false;
      if (stats != nullptr) stats->converted_pixels += width;
    }
    return true;
  }

  // A filtered sample depends on only two rows. Keep their original pixel
  // order and let the same Skia pipeline perform color conversion and the
  // horizontal/vertical interpolation; I420 nearest-neighbour scaling would
  // change thumbnails with edges or nonuniform colors.
  SkBitmap rows;
  const int row_count = std::min(height, 2);
  if (!rows.tryAllocPixels(output->info().makeWH(width, row_count))) return false;
  if (stats != nullptr) stats->scratch_bytes += rows.computeByteSize();
  // A full image outside Skia's signed-16-bit sampling range selects its
  // floating-point pipeline. Keep that choice even though our row window has
  // a small height: a float destination row forces the same highp stages.
  SkBitmap float_row;
  const bool highp = output->colorType() == kN32_SkColorType &&
                     (width >= 32767 || height >= 32767);
  if (highp) {
    if (!float_row.tryAllocPixels(output->info().makeWH(output->width(), 1)
                                   .makeColorType(kRGBA_F32_SkColorType))) return false;
    if (stats != nullptr) stats->scratch_bytes += float_row.computeByteSize();
  }
  const float scale_x = static_cast<float>(output->width()) / width;
  const float scale_y = static_cast<float>(output->height()) / height;
  const float inverse_y = 1.0f / scale_y;
  int cached_top = -1;
  sk_sp<SkImage> image;
  for (int y = 0; y < output->height(); ++y) {
    float source_y = (y + 0.5f) * inverse_y;
    source_y = std::clamp(source_y, 0.5f, height - 0.5f);
    const int top = std::clamp(static_cast<int>(std::floor(source_y - 0.5f)),
                               0, height - row_count);
    if (top != cached_top) {
      // Retire the previous borrowed image before changing its pixels. For
      // adjacent windows, keep the shared row instead of converting it again.
      image.reset();
      int first = 0;
      if (row_count == 2 && cached_top >= 0 && top == cached_top + 1) {
        std::memcpy(rows.getAddr(0, 0), rows.getAddr(0, 1), rows.rowBytes());
        first = 1;
      }
      for (int row = first; row < row_count; ++row) {
        ConvertRow(frame, rotation, top + row, width, height, argb.get());
        if (!rows.writePixels(argb_row, 0, row)) return false;
        if (stats != nullptr) stats->converted_pixels += width;
      }
      cached_top = top;
      SkPixmap pixels;
      if (!rows.peekPixels(&pixels)) return false;
      image = SkImages::RasterFromPixmap(pixels, nullptr, nullptr);
      if (image == nullptr) return false;
    }
    // The borrowed image is retained only while these source rows are valid,
    // and all pixel memory and image objects retire before this call returns.
    if (highp) float_row.eraseColor(SK_ColorTRANSPARENT);
    const auto row_info = highp ? float_row.info() : output->info().makeWH(output->width(), 1);
    auto canvas = SkCanvas::MakeRasterDirect(row_info,
        highp ? float_row.getPixels() : output->getAddr(0, y),
        highp ? float_row.rowBytes() : output->rowBytes());
    if (canvas == nullptr) return false;
    canvas->scale(scale_x, 1.0f);
    canvas->drawImage(image, 0.0f, 0.5f - (source_y - top),
                       SkSamplingOptions(SkFilterMode::kLinear));
    if (highp && !output->writePixels(float_row.pixmap(), 0, y)) return false;
  }
  output->notifyPixelsChanged();
  return true;
}
}  // namespace gesher::media
