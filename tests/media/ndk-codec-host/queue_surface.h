// Explicit in-memory native-window boundary, with real intrusive ownership and
// listener snapshots. No Windows compositor, EGL or hardware is modelled.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
struct ANativeWindow {
  std::atomic<int> refs{0};
  void incStrong(const void*) { ++refs; }
  void decStrong(const void*) {
    if (--refs == 0) delete this;
  }
  virtual ~ANativeWindow() = default;
};
namespace android {
template <class T>
class sp {
 public:
  sp() = default;
  sp(T* p) : ptr_(p) {
    if (ptr_) ptr_->incStrong(this);
  }
  sp(const sp& p) : sp(p.ptr_) {}
  sp(sp&& p) noexcept : ptr_(std::exchange(p.ptr_, nullptr)) {}
  ~sp() {
    if (ptr_) ptr_->decStrong(this);
  }
  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  bool operator!=(std::nullptr_t) const { return ptr_ != nullptr; }

 private:
  T* ptr_ = nullptr;
};
}  // namespace android
namespace gesher::media {
struct SurfaceFrame {
  int width = 0, height = 0;
  int64_t timestamp_ns = 1000;
  std::vector<uint32_t> pixels;
};
class QueueSurface : public ANativeWindow {
 public:
  QueueSurface(int w, int h) : width_(w), height_(h) { ++live; }
  ~QueueSurface() override { --live; }
  static QueueSurface* From(ANativeWindow* p) {
    return dynamic_cast<QueueSurface*>(p);
  }
  void SetDefaultSize(int w, int h) {
    width_ = w;
    height_ = h;
  }
  void SetListener(std::function<void()> fn) {
    std::lock_guard lock(mu_);
    listener_ = std::move(fn);
  }
  std::function<void()> SnapshotListener() {
    std::lock_guard lock(mu_);
    return listener_;
  }
  std::shared_ptr<const SurfaceFrame> Latest() {
    std::lock_guard lock(mu_);
    return latest_;
  }
  void Abandon() {
    std::lock_guard lock(mu_);
    abandoned_ = true;
    listener_ = {};
  }
  void Post() {
    std::function<void()> fn;
    {
      std::lock_guard lock(mu_);
      if (abandoned_) return;
      auto f = std::make_shared<SurfaceFrame>();
      f->width = width_;
      f->height = height_;
      f->pixels.resize(width_ * height_, 0);
      latest_ = std::move(f);
      fn = listener_;
    }
    if (fn) fn();
  }
  inline static std::atomic<int> live{0};

 private:
  int width_, height_;
  bool abandoned_ = false;
  std::mutex mu_;
  std::function<void()> listener_;
  std::shared_ptr<SurfaceFrame> latest_;
};
}  // namespace gesher::media
