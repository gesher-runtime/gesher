// Gesher media: a decoded frame drawn into a Surface (video_decoder.h).
#include "video_decoder.h"

#include <cmath>
#include <new>

#include <android/native_window.h>
#include <gesher/perf_trace.h>
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
    static gesher::perf::Site convert_site("video frame: I420 to RGBA into the Surface's buffer");
    const int64_t start = gesher::perf::Now();
    I420ToRgba(f, static_cast<uint32_t*>(b.bits), b.stride, b.width, b.height);
    convert_site.Add(gesher::perf::Now() - start);
  }
  static gesher::perf::Site post_site("video frame: unlockAndPost (queue to the consumer)");
  const int64_t post_start = gesher::perf::Now();
  if (timestamp_ns.has_value()) {
    if (auto* surface = QueueSurface::From(window)) {
      status = surface->UnlockAndPostWithTimestamp(*timestamp_ns);
      post_site.Add(gesher::perf::Now() - post_start);
      return status != 0 ? status : drawable ? android::OK : android::BAD_VALUE;
    }
  }
  status = ANativeWindow_unlockAndPost(window);
  post_site.Add(gesher::perf::Now() - post_start);
  return status != 0 ? status : drawable ? android::OK : android::BAD_VALUE;
}

}  // namespace gesher::media
