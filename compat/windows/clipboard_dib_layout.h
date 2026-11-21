/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Packed Windows DIB layout conversion for immutable clipboard publication.
 * Pixels, masks, palette, color space, profile and explicit V5 intent are retained.
 * The original DIB is published alongside the explicit preferred V5 layout.
 */
#pragma once
#undef NOGDI
#include <windows.h>
#include <wingdi.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace gesher::windows::clipboard_layout {
static_assert(sizeof(BITMAPCOREHEADER) == 12 && sizeof(BITMAPINFOHEADER) == 40
              && sizeof(BITMAPV4HEADER) == 108 && sizeof(BITMAPV5HEADER) == 124,
              "The clipboard layout conversion requires the actual Windows bitmap ABI");

inline bool AddSize(size_t a, size_t b, size_t* result) {
  if (a > (std::numeric_limits<size_t>::max)() - b) return false;
  *result = a + b;
  return true;
}
inline bool MultiplySize(size_t a, size_t b, size_t* result) {
  if (b && a > (std::numeric_limits<size_t>::max)() / b) return false;
  *result = a * b;
  return true;
}
template <typename T>
inline bool Load(const std::vector<uint8_t>& input, size_t offset, T* value) {
  if (offset > input.size() || sizeof(T) > input.size() - offset) return false;
  std::memcpy(value, input.data() + offset, sizeof(T));
  return true;
}

struct PackedDib {
  BITMAPV5HEADER header{};
  size_t old_header = 0, old_masks = 0, old_palette = 0;
  size_t colors = 0, pixel_offset = 0, image_bytes = 0;
  bool core = false;
};

// Parse the packed Windows header families. Unsupported headers and truncated
// ranges are explicit failures; conversion never substitutes or decodes pixels.
inline bool ParsePackedDib(const std::vector<uint8_t>& input, PackedDib* result) {
  if (!result || input.empty()) return false;
  DWORD header_size = 0;
  if (!Load(input, 0, &header_size)) return false;
  PackedDib out;
  out.old_header = header_size;
  out.core = header_size == sizeof(BITMAPCOREHEADER);
  if (out.core) {
    BITMAPCOREHEADER core{};
    if (!Load(input, 0, &core)) return false;
    out.header.bV5Width = core.bcWidth;
    out.header.bV5Height = core.bcHeight;
    out.header.bV5Planes = core.bcPlanes;
    out.header.bV5BitCount = core.bcBitCount;
    out.header.bV5Compression = BI_RGB;
  } else {
    if (header_size != 40 && header_size != 52 && header_size != 56
        && header_size != 108 && header_size != 124) return false;
    if (header_size > input.size()) return false;
    std::memcpy(&out.header, input.data(), header_size);
  }
  auto& h = out.header;
  if (h.bV5Width <= 0 || h.bV5Height == 0
      || h.bV5Height == (std::numeric_limits<LONG>::min)() || h.bV5Planes != 1) return false;
  const WORD bits = h.bV5BitCount;
  if (bits != 0 && bits != 1 && bits != 4 && bits != 8
      && bits != 16 && bits != 24 && bits != 32) return false;
  if (h.bV5Compression > 6) return false;
  if (h.bV5Compression == BI_RLE8 && bits != 8) return false;
  if (h.bV5Compression == BI_RLE4 && bits != 4) return false;
  if ((h.bV5Compression == BI_BITFIELDS || h.bV5Compression == 6)
      && bits != 16 && bits != 32) return false;
  if (h.bV5Compression != BI_JPEG && h.bV5Compression != BI_PNG && !bits) return false;
  if (h.bV5Height < 0 && h.bV5Compression != BI_RGB
      && h.bV5Compression != BI_BITFIELDS && h.bV5Compression != 6) return false;
  if (out.core && bits != 1 && bits != 4 && bits != 8 && bits != 24) return false;

  if (header_size == 40 && (h.bV5Compression == BI_BITFIELDS || h.bV5Compression == 6)) {
    out.old_masks = h.bV5Compression == 6 ? 16 : 12;
    if (!Load(input, 40, &h.bV5RedMask) || !Load(input, 44, &h.bV5GreenMask)
        || !Load(input, 48, &h.bV5BlueMask)) return false;
    if (out.old_masks == 16 && !Load(input, 52, &h.bV5AlphaMask)) return false;
  }
  out.colors = h.bV5ClrUsed;
  if (!out.colors && bits && bits <= 8) out.colors = size_t{1} << bits;
  if (bits && bits <= 8 && out.colors > (size_t{1} << bits)) return false;
  if (!MultiplySize(out.colors, out.core ? 3u : 4u, &out.old_palette)) return false;
  size_t offset = 0;
  if (!AddSize(out.old_header, out.old_masks, &offset)
      || !AddSize(offset, out.old_palette, &out.pixel_offset)
      || out.pixel_offset > input.size()) return false;
  const size_t height = static_cast<size_t>(h.bV5Height < 0 ? -h.bV5Height : h.bV5Height);
  size_t row_bits = 0, rounded = 0, row_bytes = 0, minimum_image = 0;
  if (h.bV5Compression == BI_RGB || h.bV5Compression == BI_BITFIELDS || h.bV5Compression == 6) {
    if (!MultiplySize(static_cast<size_t>(h.bV5Width), bits, &row_bits)
        || !AddSize(row_bits, 31u, &rounded)
        || !MultiplySize(rounded / 32u, 4u, &row_bytes)
        || !MultiplySize(row_bytes, height, &minimum_image)) return false;
  }
  out.image_bytes = h.bV5SizeImage ? h.bV5SizeImage : minimum_image;
  if (out.image_bytes < minimum_image || out.image_bytes > input.size() - out.pixel_offset) return false;
  if (!out.image_bytes && out.pixel_offset == input.size()) return false;
  if (header_size == 124 && (h.bV5ProfileData || h.bV5ProfileSize)) {
    if (!h.bV5ProfileData || !h.bV5ProfileSize) return false;
    size_t image_end = 0, profile_end = 0;
    if (!AddSize(out.pixel_offset, out.image_bytes, &image_end)
        || h.bV5ProfileData < image_end
        || !AddSize(h.bV5ProfileData, h.bV5ProfileSize, &profile_end)
        || profile_end > input.size()) return false;
  }
  // No color transform is performed. V4/V5 color-space, gamma, endpoints,
  // alpha, intent and profile fields stay byte-exact. Older headers have no
  // explicit intent field. Both measured Windows versions synthesize IMAGES
  // for the old INFO inputs, rather than the undocumented zero value.
  if (header_size < 108) h.bV5CSType = LCS_sRGB;
  if (header_size < 124) h.bV5Intent = LCS_GM_IMAGES;
  h.bV5Size = sizeof(BITMAPV5HEADER);
  *result = out;
  return true;
}

inline bool ToPackedV5(const std::vector<uint8_t>& input, std::vector<uint8_t>* output) {
  if (!output) return false;
  output->clear();
  PackedDib parsed;
  if (!ParsePackedDib(input, &parsed)) return false;
  size_t palette_size = 0, prefix = 0, total = 0;
  if (!MultiplySize(parsed.colors, sizeof(RGBQUAD), &palette_size)
      || !AddSize(sizeof(BITMAPV5HEADER), palette_size, &prefix)
      || !AddSize(prefix, input.size() - parsed.pixel_offset, &total)) return false;
  output->assign(total, 0);
  std::memcpy(output->data(), &parsed.header, sizeof(parsed.header));
  const size_t old_palette_start = parsed.old_header + parsed.old_masks;
  if (parsed.core) {
    for (size_t index = 0; index < parsed.colors; ++index)
      std::memcpy(output->data() + sizeof(BITMAPV5HEADER) + index * 4,
                  input.data() + old_palette_start + index * 3, 3);
  } else if (palette_size) {
    std::memcpy(output->data() + sizeof(BITMAPV5HEADER),
                input.data() + old_palette_start, palette_size);
  }
  std::memcpy(output->data() + prefix, input.data() + parsed.pixel_offset,
              input.size() - parsed.pixel_offset);
  return true;
}

}  // namespace gesher::windows::clipboard_layout
