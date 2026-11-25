// Integer, top-left pixel rectangles shared by CPU surfaces and Win32 presentation.
#pragma once

#include <algorithm>

namespace gesher {

struct PixelRect {
  int left = 0, top = 0, right = 0, bottom = 0;

  bool empty() const { return left >= right || top >= bottom; }
  static PixelRect Full(int width, int height) { return {0, 0, width, height}; }
  PixelRect Clip(int width, int height) const {
    return {std::max(0, left), std::max(0, top),
            std::min(width, right), std::min(height, bottom)};
  }
  void Unite(const PixelRect& other) {
    if (other.empty()) return;
    if (empty()) { *this = other; return; }
    left = std::min(left, other.left);
    top = std::min(top, other.top);
    right = std::max(right, other.right);
    bottom = std::max(bottom, other.bottom);
  }
};

}  // namespace gesher
