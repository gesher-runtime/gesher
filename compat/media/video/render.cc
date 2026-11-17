// Gesher media: a decoded frame drawn into a Surface (video_decoder.h).
#include "video_decoder.h"

#include <cmath>
#include <new>

#include <android/native_window.h>

#include "../surface/queue_surface.h"

namespace gesher::media {

bool RenderFrame(ANativeWindow* window, const VideoFrame& f, std::optional<int64_t> timestamp_ns) {
  constexpr int kRgba8888 = 1, kRgbx8888 = 2;
  const int w = f.visible_width(), h = f.visible_height();
  if (window == nullptr || w < 1 || h < 1) return false;
  if (ANativeWindow_setBuffersGeometry(window, w, h, kRgba8888) != 0) return false;
  ANativeWindow_Buffer b = {};
  if (ANativeWindow_lock(window, &b, nullptr) != 0) return false;
  const bool drawable = b.format == kRgba8888 || b.format == kRgbx8888;
  if (drawable) {
    I420ToRgba(f, static_cast<uint32_t*>(b.bits), b.stride, b.width, b.height);
  }
  if (timestamp_ns.has_value()) {
    if (auto* surface = QueueSurface::From(window)) {
      return surface->UnlockAndPostWithTimestamp(*timestamp_ns) == 0 && drawable;
    }
  }
  return ANativeWindow_unlockAndPost(window) == 0 && drawable;
}

}  // namespace gesher::media
