// The allocation/lifetime gate on actual Android hostgraphics objects and the
// complete QueueSurface. It needs Windows execution, not a display or codec.
#include "compat/media/surface/queue_surface.h"
#include "surface_frame_accounting.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <memory>

namespace {
using gesher::media::QueueSurface;
int checks = 0, failures = 0;
void Check(bool okay, const char* reason) {
  ++checks;
  if (!okay) { ++failures; std::fprintf(stderr, "FAIL native frame recycling: %s\n", reason); }
}
void Post(QueueSurface* surface, uint32_t pixel) {
  ANativeWindow_Buffer buffer{};
  const int status = surface->lock(&buffer, nullptr);
  Check(status == 0, "lock actual producer");
  if (status != 0) return;
  std::fill_n(static_cast<uint32_t*>(buffer.bits), static_cast<size_t>(buffer.stride) * buffer.height, pixel);
  Check(surface->unlockAndPost() == 0, "publish actual producer");
}

void ProducerOwnership() {
  auto surface = android::sp<QueueSurface>::make(64, 64);
  ANativeWindow* window = surface.get();
  ANativeWindowBuffer* native = nullptr;
  int fence = -1;
  Check(window->dequeueBuffer(window, &native, &fence) == 0 && native != nullptr,
        "dequeue actual Android native-window slot");
  if (!native) return;
  auto* pixels = static_cast<uint32_t*>(native->reserved[0]);
  pixels[0] = 0xff112233;
  ANativeWindowBuffer* excess = nullptr;
  Check(window->dequeueBuffer(window, &excess, &fence) == -ENOSYS && excess == nullptr,
        "actual native-window slot cannot be borrowed twice");
  Post(surface.get(), 0xff445566);
  Check(pixels[0] == 0xff112233, "Canvas/CPU writes leave the outstanding native slot intact");
  Check(window->queueBuffer(window, native, -1) == 0, "publish the real native slot");
  auto latest = surface->Latest();
  Check(latest && latest->pixels.front() == 0xff112233 && surface->frames() == 2,
        "each real frame has the corresponding producer's pixels");
  Check(window->queueBuffer(window, native, -1) == -EINVAL && surface->Latest() == latest,
        "duplicate return leaves the latest frame intact");
}
}

int main() {
  ProducerOwnership();
  constexpr size_t bytes = 1280u * 720u * 4u;
  const size_t initial = accounting::live;
  std::shared_ptr<const gesher::media::SurfaceFrame> held;
  {
    auto surface = android::sp<QueueSurface>::make(1280, 720);
    for (int n = 0; n < 4; ++n) Post(surface.get(), 0xff112233);
    held = surface->Latest();
    std::weak_ptr<const gesher::media::SurfaceFrame> weak = held;
    Post(surface.get(), 0xff445566);
    held.reset();
    Check(weak.expired(), "old weak frame cannot promote after final reader returns");
    accounting::Start();
    for (int n = 0; n < 120; ++n) Post(surface.get(), 0xff000000u + n);
    accounting::measuring = false;
    Check(accounting::payload_allocations == 0, "warmed actual QueueSurface allocates no pixel payloads");
    Check(accounting::live == initial + 3 * bytes, "one producer, one latest frame, one spare");
    Check(!weak.lock(), "backing reuse never resurrects the old frame");
    held = surface->Latest();
    Check(held->pixels.front() == 0xff000077 && held->pixels.back() == 0xff000077,
          "published actual bytes remain complete");
    surface->SetDefaultSize(32, 32);
    Check(accounting::live == initial + 2 * bytes, "resize releases spare immediately");
    surface->Abandon();
    Check(accounting::live == initial + bytes, "abandon leaves only retained reader pixels");
  }
  Check(held->pixels.front() == 0xff000077, "reader survives actual Surface destruction");
  held.reset();
  Check(accounting::live == initial, "last reader releases all retained pixels");
  std::printf("native surface frame recycling: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
