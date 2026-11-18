// Test-only allocation accounting and shared actual/host QueueSurface assertions.
#ifndef GESHER_SURFACE_RELEASE_ASSERTIONS_H
#define GESHER_SURFACE_RELEASE_ASSERTIONS_H
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

namespace surface_release_contract {
constexpr size_t kPixelsBytes = 3840u * 2160u * sizeof(uint32_t);
std::atomic<size_t> live_pixels{0};
struct alignas(std::max_align_t) Allocation { size_t bytes; };
}

void* operator new(size_t bytes) {
  using namespace surface_release_contract;
  auto* allocation = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + bytes));
  if (allocation == nullptr) std::abort();
  allocation->bytes = bytes;
  if (bytes == kPixelsBytes) live_pixels.fetch_add(bytes);
  return allocation + 1;
}
void operator delete(void* pointer) noexcept {
  using namespace surface_release_contract;
  if (pointer == nullptr) return;
  auto* allocation = static_cast<Allocation*>(pointer) - 1;
  if (allocation->bytes == kPixelsBytes) live_pixels.fetch_sub(allocation->bytes);
  std::free(allocation);
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, size_t) noexcept { ::operator delete(pointer); }

namespace surface_release_contract {
int checks = 0, failures = 0;
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL surface release: %s\n", reason); }
  return ok;
}
int Run() {
  using gesher::media::QueueSurface;
  const size_t baseline = live_pixels.load();
  {
    auto surface = android::sp<QueueSurface>::make(3840, 2160);
    ANativeWindow_Buffer buffer = {};
    Check(surface->lock(&buffer, nullptr) == 0, "lock full-size producer");
    static_cast<uint32_t*>(buffer.bits)[0] = 0xff123456;
    Check(surface->unlockAndPost() == 0, "publish full-size producer");
    auto held = surface->Latest();
    std::weak_ptr<const gesher::media::SurfaceFrame> weak = held;
    Check(live_pixels == baseline + 2 * kPixelsBytes, "account both real 4K pixel allocations");
    surface->Abandon();
    Check(surface->Latest() == nullptr, "abandoned queue drops latest frame");
    Check(live_pixels == baseline + kPixelsBytes, "release producer while consumer holds immutable frame");
    Check(held != nullptr && held->pixels[0] == 0xff123456, "consumer frame survives abandonment");
    held.reset();
    Check(weak.expired() && live_pixels == baseline, "last consumer frees remaining pixels before surface destruction");
    surface->Abandon();
    Check(live_pixels == baseline, "repeated abandonment is harmless");
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak_capture = capture;
    surface->SetListener([capture] {});
    capture.reset();
    Check(weak_capture.expired(), "abandoned queue cannot retain a replacement listener");
  }
  {
    auto surface = android::sp<QueueSurface>::make(3840, 2160);
    ANativeWindow_Buffer buffer = {};
    Check(surface->lock(&buffer, nullptr) == 0, "lock before abandonment");
    ANativeWindow_Buffer rejected = {};
    Check(surface->lock(&rejected, nullptr) == -ENOSYS,
          "second CPU lock preserves Surface INVALID_OPERATION");
    surface->Abandon();
    Check(surface->lock(&rejected, nullptr) == -ENOSYS,
          "an existing CPU lock is checked before abandoned consumer state");
    Check(live_pixels == baseline + kPixelsBytes, "outstanding raw CPU lock keeps pixels valid");
    static_cast<uint32_t*>(buffer.bits)[0] = 0xffabcdef;
    Check(surface->unlockAndPost() == -ENODEV, "unlock retires abandoned CPU buffer without publication");
    Check(live_pixels == baseline && surface->Latest() == nullptr, "unlock frees deferred CPU storage");
    Check(surface->lock(&rejected, nullptr) == -ENODEV,
          "fresh CPU lock on abandoned consumer reports NO_INIT");
  }
  for (bool queue : {false, true}) {
    auto surface = android::sp<QueueSurface>::make(3840, 2160);
    ANativeWindow* window = surface.get();
    ANativeWindowBuffer* buffer = nullptr;
    int fence = -1;
    Check(window->dequeueBuffer(window, &buffer, &fence) == 0, "dequeue before abandonment");
    surface->Abandon();
    Check(live_pixels == baseline + kPixelsBytes, "outstanding dequeued producer keeps its storage");
    Check(buffer != nullptr && buffer->width == 3840, "dequeued buffer remains usable");
    int result = queue ? window->queueBuffer(window, buffer, fence) : window->cancelBuffer(window, buffer, fence);
    Check(result == (queue ? -ENODEV : 0), "producer return retires abandoned buffer");
    Check(live_pixels == baseline && surface->Latest() == nullptr, "producer return frees pixels without reviving queue");
  }
  {
    auto surface = android::sp<QueueSurface>::make(3840, 2160);
    ANativeWindow* window = surface.get();
    ANativeWindowBuffer *first = nullptr, *second = nullptr;
    int fence = -1;
    Check(window->dequeueBuffer(window, &first, &fence) == 0, "dequeue original format");
    Check(window->perform(window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, HAL_PIXEL_FORMAT_BGRA_8888) == 0,
          "change format while original producer remains outstanding");
    Check(window->dequeueBuffer(window, &second, &fence) == 0 && first != second,
          "dequeue new format has separate storage");
    surface->Abandon();
    Check(live_pixels == baseline + 2 * kPixelsBytes, "abandon preserves both outstanding generations");
    Check(window->queueBuffer(window, first, fence) == -ENODEV, "return old generation after abandon");
    Check(live_pixels == baseline + kPixelsBytes, "old generation freed without retaining it as current");
    Check(window->cancelBuffer(window, second, fence) == 0, "return new generation after abandon");
    Check(live_pixels == baseline, "all outstanding generations freed before surface destruction");
  }
  {
    auto surface = android::sp<QueueSurface>::make(3840, 2160);
    ANativeWindow* window = surface.get();
    ANativeWindowBuffer* dequeued = nullptr;
    ANativeWindow_Buffer locked = {};
    int fence = -1;
    Check(surface->lock(&locked, nullptr) == 0, "lock shared producer storage");
    Check(window->dequeueBuffer(window, &dequeued, &fence) == 0, "dequeue shares locked storage");
    surface->Abandon();
    Check(surface->unlockAndPost() == -ENODEV && live_pixels == baseline + kPixelsBytes,
          "unlock preserves outstanding dequeue ownership");
    Check(window->cancelBuffer(window, dequeued, fence) == 0 && live_pixels == baseline,
          "final dequeue return releases shared storage");
  }
  std::printf("surface release contracts: %d checks, %d failures; 4K queue bytes=%zu\n",
              checks, failures, 2 * kPixelsBytes);
  return failures;
}
}
#endif
