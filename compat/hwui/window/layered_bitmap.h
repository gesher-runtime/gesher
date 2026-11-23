// A layered window's 32-bit, top-down DIB. Used and destroyed on its owner
// thread only; dimensions are physical pixels, independent of window DPI.
#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace gesher {

class LayeredBitmap {
 public:
  LayeredBitmap() = default;
  ~LayeredBitmap() { Reset(); }
  LayeredBitmap(const LayeredBitmap&) = delete;
  LayeredBitmap& operator=(const LayeredBitmap&) = delete;

  bool Copy(const uint8_t* source, int width, int height) {
    if (source == nullptr || width <= 0 || height <= 0) return false;
    // Finish any GDI use before modifying or replacing the selected DIB.
    GdiFlush();
    if ((width != width_ || height != height_) && !Resize(width, height)) return false;
    std::memcpy(bits_, source, static_cast<size_t>(width_) * height_ * 4);
    return true;
  }

  bool CopyRect(const uint8_t* source, int width, int height,
                int left, int top, int right, int bottom) {
    if (source == nullptr || width <= 0 || height <= 0) return false;
    if (width != width_ || height != height_ || bits_ == nullptr ||
        left < 0 || top < 0 || right > width || bottom > height ||
        left >= right || top >= bottom ||
        (left == 0 && top == 0 && right == width && bottom == height)) {
      return Copy(source, width, height);
    }
    GdiFlush();
    auto* dst = static_cast<uint8_t*>(bits_);
    const size_t stride_bytes = static_cast<size_t>(width_) * 4;
    const size_t row_bytes = static_cast<size_t>(right - left) * 4;
    const size_t x_bytes = static_cast<size_t>(left) * 4;
    for (int y = top; y < bottom; ++y) {
      const size_t offset = static_cast<size_t>(y) * stride_bytes + x_bytes;
      std::memcpy(dst + offset, source + offset, row_bytes);
    }
    return true;
  }

  HDC dc() const { return dc_; }

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

struct LayeredDirtyEntry {
  const void* window = nullptr;
  int width = 0, height = 0;
  int left = 0, top = 0, right = 0, bottom = 0;
  bool has_partial = false;
  bool pending_full = false;
  bool dimmed = false;
};

struct LayeredDirtyRegistry {
  std::mutex mutex;
  std::vector<LayeredDirtyEntry> entries;
};

inline LayeredDirtyRegistry& GetLayeredDirtyRegistry() {
  static auto* registry = new LayeredDirtyRegistry();
  return *registry;
}

inline LayeredDirtyEntry* FindLayeredDirtyEntry(std::vector<LayeredDirtyEntry>& entries,
                                                const void* window, bool create) {
  for (auto& entry : entries) {
    if (entry.window == window) return &entry;
  }
  if (!create) return nullptr;
  entries.push_back(LayeredDirtyEntry{window});
  return &entries.back();
}

inline void RecordLayeredPartial(const void* window, int width, int height,
                                 int left, int top, int right, int bottom) {
  auto& reg = GetLayeredDirtyRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  LayeredDirtyEntry* e = FindLayeredDirtyEntry(reg.entries, window, true);
  if (width <= 0 || height <= 0 || left < 0 || top < 0 ||
      right > width || bottom > height || left >= right || top >= bottom ||
      (left == 0 && top == 0 && right == width && bottom == height) ||
      e->pending_full || e->dimmed ||
      (e->has_partial && (e->width != width || e->height != height))) {
    e->pending_full = true;
    e->has_partial = false;
    return;
  }
  if (e->has_partial) {
    e->left = std::min(e->left, left);
    e->top = std::min(e->top, top);
    e->right = std::max(e->right, right);
    e->bottom = std::max(e->bottom, bottom);
  } else {
    e->width = width;
    e->height = height;
    e->left = left;
    e->top = top;
    e->right = right;
    e->bottom = bottom;
    e->has_partial = true;
  }
}

inline void RecordLayeredFull(const void* window) {
  auto& reg = GetLayeredDirtyRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  LayeredDirtyEntry* e = FindLayeredDirtyEntry(reg.entries, window, true);
  e->pending_full = true;
  e->has_partial = false;
}

inline void SetLayeredDimmed(const void* window, bool dimmed) {
  auto& reg = GetLayeredDirtyRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  LayeredDirtyEntry* e = FindLayeredDirtyEntry(reg.entries, window, true);
  e->dimmed = dimmed;
  e->pending_full = true;
  e->has_partial = false;
}

inline void ResetLayeredTranslucencyDirty(const void* window) {
  auto& reg = GetLayeredDirtyRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto it = reg.entries.begin(); it != reg.entries.end(); ++it) {
    if (it->window != window) continue;
    if (it->dimmed) {
      it->pending_full = true;
      it->has_partial = false;
    } else {
      reg.entries.erase(it);
    }
    return;
  }
}

inline void EraseLayeredDirty(const void* window) {
  auto& reg = GetLayeredDirtyRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  reg.entries.erase(
      std::remove_if(reg.entries.begin(), reg.entries.end(),
                     [window](const LayeredDirtyEntry& e) { return e.window == window; }),
      reg.entries.end());
}

inline bool TakeLayeredDirty(const void* window, int width, int height, int out[4]) {
  auto& reg = GetLayeredDirtyRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  LayeredDirtyEntry* e = FindLayeredDirtyEntry(reg.entries, window, false);
  if (e == nullptr) return false;
  const bool partial = !e->dimmed && !e->pending_full && e->has_partial &&
                       e->width == width && e->height == height &&
                       e->left >= 0 && e->top >= 0 &&
                       e->right <= width && e->bottom <= height &&
                       e->left < e->right && e->top < e->bottom;
  if (partial) {
    out[0] = e->left;
    out[1] = e->top;
    out[2] = e->right;
    out[3] = e->bottom;
  }
  e->pending_full = false;
  e->has_partial = false;
  return partial;
}

}  // namespace gesher
