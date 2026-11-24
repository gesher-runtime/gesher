// A layered window's 32-bit, top-down DIB. Used and destroyed on its owner
// thread only; dimensions are physical pixels, independent of window DPI.
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "surface_damage.h"

namespace gesher {

class LayeredBitmap {
 public:
  LayeredBitmap() = default;
  ~LayeredBitmap() { Reset(); }
  LayeredBitmap(const LayeredBitmap&) = delete;
  LayeredBitmap& operator=(const LayeredBitmap&) = delete;

  bool Copy(const uint8_t* source, int width, int height, const PixelRect* dirty = nullptr) {
    if (source == nullptr || width <= 0 || height <= 0) return false;
    // Finish any GDI use before modifying or replacing the selected DIB.
    GdiFlush();
    const bool retained = Matches(width, height);
    if (!retained && !Resize(width, height)) return false;
    PixelRect copy = retained && dirty != nullptr ? dirty->Clip(width, height) : PixelRect{};
    if (copy.empty()) copy = PixelRect::Full(width, height);
    if (copy.left == 0 && copy.right == width_) {
      const size_t offset = static_cast<size_t>(copy.top) * width_ * 4;
      std::memcpy(static_cast<uint8_t*>(bits_) + offset, source + offset,
                  static_cast<size_t>(copy.bottom - copy.top) * width_ * 4);
      return true;
    }
    for (int y = copy.top; y < copy.bottom; ++y) {
      const size_t offset = (static_cast<size_t>(y) * width_ + copy.left) * 4;
      std::memcpy(static_cast<uint8_t*>(bits_) + offset, source + offset,
                  static_cast<size_t>(copy.right - copy.left) * 4);
    }
    return true;
  }

  HDC dc() const { return dc_; }
  bool Matches(int width, int height) const {
    return bits_ != nullptr && width == width_ && height == height_;
  }

  void Reset() {
    if (dc_ != nullptr) {
      GdiFlush();
      SelectObject(dc_, original_);
      DeleteObject(bitmap_);
      DeleteDC(dc_);
    }
    dc_ = nullptr;
    bitmap_ = nullptr;
    original_ = nullptr;
    bits_ = nullptr;
    width_ = height_ = 0;
  }

 private:
  bool Resize(int width, int height) {
    HDC dc = dc_ != nullptr ? dc_ : CreateCompatibleDC(nullptr);
    if (dc == nullptr) return false;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
      if (bitmap != nullptr) DeleteObject(bitmap);
      if (dc_ == nullptr) DeleteDC(dc);
      return false;
    }
    HGDIOBJ previous = SelectObject(dc, bitmap);
    if (previous == nullptr || previous == HGDI_ERROR) {
      DeleteObject(bitmap);
      if (dc_ == nullptr) DeleteDC(dc);
      return false;
    }
    if (dc_ == nullptr) {
      dc_ = dc;
      original_ = previous;
    } else {
      DeleteObject(bitmap_);  // now deselected, safe to retire
    }
    bitmap_ = bitmap;
    bits_ = bits;
    width_ = width;
    height_ = height;
    return true;
  }

  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ original_ = nullptr;
  void* bits_ = nullptr;
  int width_ = 0, height_ = 0;
};

}  // namespace gesher
