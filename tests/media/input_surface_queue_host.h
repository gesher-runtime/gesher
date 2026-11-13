// Declared Linux fixture boundary: intrusive Surface ownership, pixel queue
// storage and publication. Production input_surface.cc is compiled unchanged.
// Windows uses the real QueueSurface/hostgraphics with the same assertions.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

struct ANativeWindow_Buffer {
  int width = 0, height = 0, stride = 0;
  void* bits = nullptr;
};
struct ARect;
namespace android {
class Surface {
 public:
  void incStrong(const void*) const { ++references_; }
  void decStrong(const void*) const { if (--references_ == 0) delete this; }
 protected:
  virtual ~Surface() = default;
 private:
  mutable std::atomic<int> references_{0};
};
template<class T> class sp {
 public:
  sp() = default;
  sp(T* value) : value_(value) { Acquire(); }
  sp(const sp& other) : value_(other.get()) { Acquire(); }
  template<class U> sp(const sp<U>& other) : value_(other.get()) { Acquire(); }
  ~sp() { if (value_) value_->decStrong(this); }
  sp& operator=(const sp& other) {
    sp copy(other); std::swap(value_, copy.value_); return *this;
  }
  T* get() const { return value_; }
  T* operator->() const { return value_; }
  bool operator!=(std::nullptr_t) const { return value_ != nullptr; }
 private:
  void Acquire() { if (value_) value_->incStrong(this); }
  T* value_ = nullptr;
};
}  // namespace android

namespace gesher::media {
struct SurfaceFrame {
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;
  int64_t timestamp_ns = 1000;
};
class QueueSurface : public android::Surface {
 public:
  QueueSurface(int w, int h) : width_(w), height_(h), pixels_(w * h) {}
  void SetListener(std::function<void()> listener) {
    std::lock_guard<std::mutex> lock(mu_); listener_ = std::move(listener);
  }
  std::function<void()> SnapshotListener() const {
    std::lock_guard<std::mutex> lock(mu_); return listener_;
  }
  std::shared_ptr<const SurfaceFrame> Latest() const {
    std::lock_guard<std::mutex> lock(mu_); return latest_;
  }
  uint64_t frames() const { std::lock_guard<std::mutex> lock(mu_); return frames_; }
  void Abandon() {
    std::lock_guard<std::mutex> lock(mu_); abandoned_ = true; listener_ = nullptr;
  }
  int lock(ANativeWindow_Buffer* buffer, ARect*) {
    std::lock_guard<std::mutex> lock(mu_);
    if (abandoned_ || locked_) return -1;
    locked_ = true;
    *buffer = {width_, height_, width_, pixels_.data()};
    return 0;
  }
  int unlockAndPost() {
    std::function<void()> listener;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!locked_) return -1;
      locked_ = false;
      if (abandoned_) return 0;
      auto frame = std::make_shared<SurfaceFrame>();
      frame->width = width_; frame->height = height_; frame->pixels = pixels_;
      latest_ = std::move(frame);
      ++frames_;
      listener = listener_;
    }
    if (listener) listener();
    return 0;
  }
 private:
  mutable std::mutex mu_;
  int width_, height_;
  std::vector<uint32_t> pixels_;
  std::shared_ptr<const SurfaceFrame> latest_;
  std::function<void()> listener_;
  uint64_t frames_ = 0;
  bool abandoned_ = false, locked_ = false;
};
}  // namespace gesher::media
