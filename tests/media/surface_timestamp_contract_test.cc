// Actual QueueSurface and AOSP native-window dispatch; no display is required.
#include "compat/media/surface/queue_surface.h"
#include <cstdio>
#include <cstdint>
#include <system/window.h>

namespace {
int checks = 0, failures = 0;
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL surface timestamp: %s\n", reason); }
  return ok;
}
void Post(gesher::media::QueueSurface* surface) {
  ANativeWindow_Buffer buffer = {};
  if (!Check(surface->lock(&buffer, nullptr) == 0, "lock a real producer buffer")) return;
  static_cast<uint32_t*>(buffer.bits)[0] = 0xff123456;
  Check(surface->unlockAndPost() == 0, "publish a real producer buffer");
}
}

int main() {
  using gesher::media::QueueSurface;
  auto surface = android::sp<QueueSurface>::make(2, 2);
  ANativeWindow* window = surface.get();
  int callbacks = 0;
  surface->SetListener([&] { ++callbacks; Check(surface->Latest() != nullptr, "listener sees publication"); });
  int64_t start = systemTime(SYSTEM_TIME_MONOTONIC);
  Post(surface.get());
  int64_t end = systemTime(SYSTEM_TIME_MONOTONIC);
  auto frame = surface->Latest();
  Check(frame != nullptr && frame->timestamp_ns >= start && frame->timestamp_ns <= end,
        "default timestamp is monotonic auto time");
  Check(native_window_set_buffers_timestamp(window, 0) == 0, "set explicit zero through native-window API");
  Post(surface.get());
  Check(surface->Latest()->timestamp_ns == 0, "zero is an explicit timestamp");
  Post(surface.get());
  Check(surface->Latest()->timestamp_ns == 0, "explicit zero persists across frames");
  Check(native_window_set_buffers_timestamp(window, 123456789) == 0, "set positive timestamp");
  Post(surface.get());
  auto explicit_frame = surface->Latest();
  Check(explicit_frame->timestamp_ns == 123456789, "positive timestamp forwarded");
  ANativeWindowBuffer* buffer = nullptr;
  int fence = -1;
  Check(window->dequeueBuffer(window, &buffer, &fence) == 0 && buffer != nullptr, "dequeue native buffer");
  Check(window->queueBuffer(window, buffer, fence) == 0, "queue native buffer");
  Check(surface->Latest()->timestamp_ns == 123456789, "explicit timestamp persists for native queue");
  Check(explicit_frame->timestamp_ns == 123456789, "published frame stays immutable");
  Check(native_window_set_buffers_timestamp(window, NATIVE_WINDOW_TIMESTAMP_AUTO) == 0,
        "return to the Android auto sentinel");
  start = systemTime(SYSTEM_TIME_MONOTONIC);
  Post(surface.get());
  end = systemTime(SYSTEM_TIME_MONOTONIC);
  frame = surface->Latest();
  Check(frame->timestamp_ns >= start && frame->timestamp_ns <= end,
        "auto sentinel is replaced, never emitted literally");
  Check(callbacks == 6 && surface->frames() == 6, "one callback and frame per publication");
  surface->Abandon();
  ANativeWindow_Buffer abandoned = {};
  Check(surface->lock(&abandoned, nullptr) != 0, "abandoned surface still rejects production");
  std::printf("surface timestamp contracts: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
