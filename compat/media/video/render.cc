// Gesher media: a decoded frame drawn into a Surface (video_decoder.h).
#include <algorithm>

#include "video_decoder.h"

// The ANativeWindow functions dalvikvm defines (docs/graphics.md).
struct ANativeWindow;
struct ARect {
  int32_t left, top, right, bottom;
};
struct ANativeWindow_Buffer {
  int32_t width, height, stride, format;
  void* bits;
  uint32_t reserved[6];
};
extern "C" {
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t w, int32_t h, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* buffer, ARect* bounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window);
}

namespace gesher::media {

bool RenderFrame(ANativeWindow* window, const VideoFrame& f) {
  constexpr int kRgba8888 = 1, kRgbx8888 = 2;
  const int w = f.visible_width(), h = f.visible_height();
  if (window == nullptr || w < 1 || h < 1) return false;
  if (ANativeWindow_setBuffersGeometry(window, w, h, kRgba8888) != 0) return false;
  ANativeWindow_Buffer b = {};
  if (ANativeWindow_lock(window, &b, nullptr) != 0) return false;
  if (b.format == kRgba8888 || b.format == kRgbx8888) {
    I420ToRgba(f, static_cast<uint32_t*>(b.bits), b.stride, b.width, b.height);
  }
  return ANativeWindow_unlockAndPost(window) == 0;
}

}  // namespace gesher::media
