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
struct alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) Allocation { size_t bytes; };
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
    Check(surface->lock(&locked, nullptr) == 0, "lock CPU producer storage");
    Check(window->dequeueBuffer(window, &dequeued, &fence) == 0 &&
              dequeued != nullptr && dequeued->reserved[0] != locked.bits,
          "dequeue cannot alias a CPU lock");
    surface->Abandon();
    Check(live_pixels == baseline + 2 * kPixelsBytes, "both producers retain separate storage");
    Check(surface->unlockAndPost() == -ENODEV && live_pixels == baseline + kPixelsBytes,
          "unlock releases only the CPU producer storage");
    Check(window->cancelBuffer(window, dequeued, fence) == 0 && live_pixels == baseline,
          "final dequeue return releases its storage");
  }
  {
    auto surface = android::sp<QueueSurface>::make(4, 2);
    auto other = android::sp<QueueSurface>::make(4, 2);
    ANativeWindow* window = surface.get();
    ANativeWindow* foreign_window = other.get();
    ANativeWindowBuffer *first = nullptr, *second = nullptr, *foreign = nullptr, *extra = nullptr;
    int fence = 123;
    Check(window->dequeueBuffer(window, &first, &fence) == 0 && first != nullptr && fence == -1,
          "dequeue first writable buffer");
    Check(window->dequeueBuffer(window, &second, &fence) == 0 && second != nullptr && second != first,
          "same geometry native dequeues own distinct slots");
    Check(window->dequeueBuffer(window, &extra, &fence) == -EAGAIN && extra == nullptr,
          "initial writes have bounded capacity and use WOULD_BLOCK");
    Check(foreign_window->dequeueBuffer(foreign_window, &foreign, &fence) == 0 && foreign != nullptr,
          "other window owns its own producer");
    Check(window->queueBuffer(window, foreign, fence) == -EINVAL &&
              window->cancelBuffer(window, foreign, fence) == -EINVAL && surface->frames() == 0,
          "foreign queue and cancel leave local producers and frame count alone");
    if (first != nullptr && second != nullptr) {
      static_cast<uint32_t*>(first->reserved[0])[0] = 0xff112233;
      static_cast<uint32_t*>(second->reserved[0])[0] = 0xff445566;
      Check(window->queueBuffer(window, first, fence) == 0 &&
                surface->Latest() && surface->Latest()->pixels[0] == 0xff112233,
            "queue publishes only the returned producer pixels");
      Check(window->queueBuffer(window, first, fence) == -EINVAL,
            "returning the same producer twice does not republish");
      Check(window->dequeueBuffer(window, &extra, &fence) == -ENOSYS && extra == nullptr,
            "default limit is one outstanding producer after the first queue");
      Check(window->cancelBuffer(window, second, fence) == 0 && surface->frames() == 1,
            "cancel returns a slot without publishing pixels");
      Check(window->dequeueBuffer(window, &extra, &fence) == 0 && extra != nullptr,
            "a returned slot allows the next producer to make progress");
      if (extra != nullptr) {
        auto published = surface->Latest();
        static_cast<uint32_t*>(extra->reserved[0])[0] = 0xffaabbcc;
        Check(window->cancelBuffer(window, extra, fence) == 0 &&
                  published && published->pixels[0] == 0xff112233,
              "cancelled writes cannot modify the published frame");
        ANativeWindow_Buffer cpu = {};
        ARect damage{1, 0, 2, 1};
        const int status = surface->lock(&cpu, &damage);
        Check(status == 0 && damage.left == 0 && damage.top == 0 &&
                  damage.right == 4 && damage.bottom == 2,
              "cancelled storage requires a complete CPU repaint");
        if (status == 0) surface->unlockAndPost();
      }
    }
    Check(window->perform(window, NATIVE_WINDOW_SET_BUFFER_COUNT, 3) == 0,
          "producer can negotiate two writable slots after first publication");
    ANativeWindowBuffer *negotiated_a = nullptr, *negotiated_b = nullptr;
    Check(window->dequeueBuffer(window, &negotiated_a, &fence) == 0 &&
              window->dequeueBuffer(window, &negotiated_b, &fence) == 0 &&
              negotiated_a != nullptr && negotiated_b != nullptr && negotiated_a != negotiated_b,
          "negotiated count allows two actual independent writes");
    Check(window->perform(window, NATIVE_WINDOW_SET_BUFFER_COUNT, 0) == -EINVAL,
          "cannot reduce capacity below the slots currently owned by producers");
    if (negotiated_a != nullptr) window->cancelBuffer(window, negotiated_a, fence);
    if (negotiated_b != nullptr) window->cancelBuffer(window, negotiated_b, fence);
    Check(window->perform(window, NATIVE_WINDOW_SET_BUFFER_COUNT, 0) == 0,
          "reset restores the default producer capacity");
    Check(window->perform(window, NATIVE_WINDOW_SET_BUFFER_COUNT, 4) == -EINVAL,
          "producer count above the backend capacity is rejected");
    ANativeWindowBuffer *reset = nullptr, *over = nullptr;
    Check(window->dequeueBuffer(window, &reset, &fence) == 0 &&
              window->dequeueBuffer(window, &over, &fence) == -ENOSYS && over == nullptr,
          "producer uses the restored one-slot limit");
    if (reset != nullptr) window->cancelBuffer(window, reset, fence);
    if (over != nullptr) window->cancelBuffer(window, over, fence);
    if (foreign != nullptr)
      Check(foreign_window->cancelBuffer(foreign_window, foreign, fence) == 0,
            "a rejected foreign return leaves its real owner able to cancel");
  }
  {
    auto surface = android::sp<QueueSurface>::make(4, 2);
    ANativeWindow* window = surface.get();
    ANativeWindow_Buffer cpu = {};
    ANativeWindowBuffer* native = nullptr;
    int fence = -1;
    const int locked = surface->lock(&cpu, nullptr);
    Check(locked == 0, "take writable CPU slot");
    const int dequeued = window->dequeueBuffer(window, &native, &fence);
    Check(dequeued == 0 && native != nullptr, "take another writable native slot");
    if (locked == 0 && dequeued == 0 && native != nullptr) {
      static_cast<uint32_t*>(cpu.bits)[0] = 0xff123456;
      static_cast<uint32_t*>(native->reserved[0])[0] = 0xfffedcba;
      Check(window->queueBuffer(window, native, fence) == 0 &&
                surface->Latest() && surface->Latest()->pixels[0] == 0xfffedcba,
            "native slot publishes while CPU owns a separate writable slot");
      const auto earlier = surface->Latest();
      Check(surface->unlockAndPost() == 0 &&
                surface->Latest() && surface->Latest()->pixels[0] == 0xff123456 &&
                earlier && earlier->pixels[0] == 0xfffedcba,
            "CPU unlock publishes its own pixels and retains immutable native frame");
    }
  }
  std::printf("surface release contracts: %d checks, %d failures; 4K queue bytes=%zu\n",
              checks, failures, 2 * kPixelsBytes);
  return failures;
}
}
#endif
