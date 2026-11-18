// Gesher media: a decoded frame drawn into a Surface (video_decoder.h).
#include "video_decoder.h"

#include <cmath>
#include <new>

#include <android/native_window.h>
#include <utils/Errors.h>

#include "../surface/queue_surface.h"

namespace gesher::media {

int RenderFrame(ANativeWindow* window, const VideoFrame& f, std::optional<int64_t> timestamp_ns) {
  constexpr int kRgba8888 = 1, kRgbx8888 = 2;
  const int w = f.visible_width(), h = f.visible_height();
  if (window == nullptr) return android::NO_INIT;
  if (w < 1 || h < 1) return android::BAD_VALUE;
  int status = ANativeWindow_setBuffersGeometry(window, w, h, kRgba8888);
  if (status != 0) return status;
  ANativeWindow_Buffer b = {};
  status = ANativeWindow_lock(window, &b, nullptr);
  if (status != 0) return status;
  const bool drawable = b.format == kRgba8888 || b.format == kRgbx8888;
  if (drawable) {
    I420ToRgba(f, static_cast<uint32_t*>(b.bits), b.stride, b.width, b.height);
  }
  if (timestamp_ns.has_value()) {
    if (auto* surface = QueueSurface::From(window)) {
      status = surface->UnlockAndPostWithTimestamp(*timestamp_ns);
      return status != 0 ? status : drawable ? android::OK : android::BAD_VALUE;
    }
  }
  status = ANativeWindow_unlockAndPost(window);
  return status != 0 ? status : drawable ? android::OK : android::BAD_VALUE;
}

}  // namespace gesher::media
