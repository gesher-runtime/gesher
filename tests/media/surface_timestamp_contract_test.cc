// Actual QueueSurface timing and dataspace contracts. No display is required.
#include "compat/media/surface/queue_surface.h"
#include "compat/hwui/window/gesher_native_window.h"
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <system/window.h>
#include "surface_release_assertions.h"

namespace {
int checks = 0, failures = 0;
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL surface timestamp: %s\n", reason); }
  return ok;
}
void Post(gesher::media::QueueSurface* surface,
          std::optional<int64_t> timestamp_ns = std::nullopt) {
  ANativeWindow_Buffer buffer = {};
  if (!Check(surface->lock(&buffer, nullptr) == 0, "lock a real producer buffer")) return;
  static_cast<uint32_t*>(buffer.bits)[0] = 0xff123456;
  const int result = timestamp_ns.has_value()
      ? surface->UnlockAndPostWithTimestamp(*timestamp_ns)
      : surface->unlockAndPost();
  Check(result == 0, "publish a real producer buffer");
}

void CheckDataspace() {
  using gesher::media::QueueSurface;
  auto surface = android::sp<QueueSurface>::make(2, 2);
  ANativeWindow* window = surface.get();
  Check(ANativeWindow_getBuffersDefaultDataSpace(window) == HAL_DATASPACE_UNKNOWN &&
        ANativeWindow_getBuffersDataSpace(window) == HAL_DATASPACE_UNKNOWN,
        "initial consumer and producer dataspaces are unknown");
  Check(ANativeWindow_setBuffersDataSpace(window, HAL_DATASPACE_V0_SRGB) == 0 &&
        ANativeWindow_getBuffersDataSpace(window) == HAL_DATASPACE_V0_SRGB &&
        ANativeWindow_getBuffersDefaultDataSpace(window) == HAL_DATASPACE_UNKNOWN,
        "the producer can change its tag independently of the consumer default");
  Post(surface.get());
  auto first = surface->Latest();
  Check(first != nullptr && first->dataspace == HAL_DATASPACE_V0_SRGB,
        "a CPU-posted frame carries the producer's dataspace");

  ANativeWindowBuffer* buffer = nullptr;
  int fence = -1;
  if (Check(window->dequeueBuffer(window, &buffer, &fence) == 0 && buffer != nullptr,
            "dequeue a native producer buffer")) {
    Check(ANativeWindow_setBuffersDataSpace(window, HAL_DATASPACE_V0_SRGB_LINEAR) == 0,
          "change the producer tag after dequeue");
    Check(window->queueBuffer(window, buffer, fence) == 0 &&
          surface->Latest()->dataspace == HAL_DATASPACE_V0_SRGB_LINEAR,
          "native queue snapshots the tag at publication");
  }
  Check(first != nullptr && first->dataspace == HAL_DATASPACE_V0_SRGB,
        "changing the producer tag leaves an earlier frame immutable");

  constexpr uint32_t pixels[] = {0xff123456, 0xff234567, 0xff345678, 0xff456789};
  Check(surface->PostFrame(2, 2, 2, pixels, 321, 1, HAL_DATASPACE_V0_SRGB) == 0 &&
        surface->Latest()->dataspace == HAL_DATASPACE_V0_SRGB &&
        surface->Latest()->pixels[0] == pixels[0] &&
        ANativeWindow_getBuffersDataSpace(window) == HAL_DATASPACE_V0_SRGB_LINEAR,
        "a forwarded publication carries its own tag and leaves local producer state alone");
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
  Post(surface.get(), 0);
  Check(surface->Latest()->timestamp_ns == 0, "one frame overrides producer timestamp with zero");
  Post(surface.get(), -1234);
  Check(surface->Latest()->timestamp_ns == -1234, "one frame preserves a signed explicit timestamp");
  Post(surface.get(), INT64_MIN);
  Check(surface->Latest()->timestamp_ns == INT64_MIN,
        "codec timestamp preserves INT64_MIN without invoking native producer AUTO");
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
  Check(callbacks == 9 && surface->frames() == 9, "one callback and frame per publication");
  ANativeWindow_Buffer abandoned = {};
  Check(surface->lock(&abandoned, nullptr) == 0, "acquire producer before abandonment");
  Check(surface->lock(&abandoned, nullptr) == -ENOSYS, "second CPU lock returns INVALID_OPERATION");
  surface->Abandon();
  Check(surface->lock(&abandoned, nullptr) == -ENOSYS, "CPU lock admission retains Surface's precedence after abandonment");
  Check(surface->unlockAndPost() == -ENODEV, "pending producer observes NO_INIT when posting after abandonment");
  Check(surface->lock(&abandoned, nullptr) == -ENODEV, "already abandoned consumer rejects a fresh lock with NO_INIT");
  CheckDataspace();
  std::printf("surface timestamp contracts: %d checks, %d failures\n", checks, failures);
  return surface_release_contract::Run() != 0 || failures != 0;
}
