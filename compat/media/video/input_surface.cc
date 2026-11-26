// Gesher media: input surfaces over QueueSurface (input_surface.h; ADR 0059).
// Built into hwui with the surfaces it shares the QueueSurface with
// (cmake/aosp/media.cmake).

#include "input_surface.h"

#include <android_runtime/android_view_Surface.h>

#include <memory>
#include <mutex>

#include "../surface/queue_surface.h"

namespace gesher::media {
namespace {

struct InputState {
  // Admission, invocation and owner replacement use the same lock. A callback
  // may retire its own lease and install its successor without a lock inversion.
  // Its local callback copy keeps the old invocation's captures alive.
  std::recursive_mutex mu;
  bool abandoned = false;
  uint64_t owner = 0, next_owner = 1;
  InputFrameCallback cb;
};

// Native clients retain the producer through ANativeWindow's reference count.
class NativePersistentProducer final : public QueueSurface {
 public:
  NativePersistentProducer() : QueueSurface(1, 1) {}

  const std::shared_ptr<InputState> state = std::make_shared<InputState>();
};

struct InputQueue {
  InputQueue(int width, int height)
      : state(std::make_shared<InputState>()), surface(new QueueSurface(width, height)) {}
  explicit InputQueue(NativePersistentProducer* producer)
      : state(producer->state), surface(producer), native_owned(true) {}

  // Called under state->mu before this lease's callback is installed.
  void Listen(uint64_t owner) {
    // Capture only the queue's raw pointer and callback state, not its owner.
    // QueueSurface invokes the listener after releasing its own queue mutex.
    QueueSurface* raw = surface.get();
    std::shared_ptr<InputState> s = state;
    surface->SetListener([raw, s, owner] {
      std::lock_guard<std::recursive_mutex> lock(s->mu);
      // A producer may have copied the listener before detachment. Its old
      // frame must not be delivered to an encoder attached in the meantime.
      if (s->owner != owner) return;
      InputFrameCallback cb = s->cb;
      if (!cb) return;
      std::shared_ptr<const SurfaceFrame> f = raw->Latest();
      if (f == nullptr || f->pixels.empty()) return;
      cb(f->pixels.data(), f->width, f->height, f->timestamp_ns);
    });
  }

  ~InputQueue() {
    // The native producer's own references may outlive an adapter or lease.
    if (native_owned) return;
    std::lock_guard<std::recursive_mutex> lock(state->mu);
    state->abandoned = true;
    state->cb = nullptr;
    surface->Abandon();
  }

  jobject NewJavaSurface(JNIEnv* env) {
    android::sp<android::Surface> s = surface;
    return android::android_view_Surface_createFromSurface(env, s);
  }

  const std::shared_ptr<InputState> state;
  const android::sp<QueueSurface> surface;
  const bool native_owned = false;
};

class QueueInputSurface final : public InputSurface {
 public:
  QueueInputSurface(std::shared_ptr<InputQueue> queue, uint64_t owner, bool persistent)
      : queue_(std::move(queue)), owner_(owner), persistent_(persistent) {}
  ~QueueInputSurface() override { Abandon(); }

  jobject NewJavaSurface(JNIEnv* env) override { return queue_->NewJavaSurface(env); }
  ANativeWindow* NativeWindow() const override { return queue_->surface.get(); }

  void SetCallback(InputFrameCallback cb) override {
    std::lock_guard<std::recursive_mutex> lock(queue_->state->mu);
    if (!queue_->state->abandoned && queue_->state->owner == owner_)
      queue_->state->cb = std::move(cb);
  }

  uint64_t frames() const override { return queue_->surface->frames(); }
  bool persistent() const override { return persistent_; }

  void Abandon() override {
    std::lock_guard<std::recursive_mutex> lock(queue_->state->mu);
    // An old, already retired lease must not clear a successor's callback.
    if (queue_->state->owner != owner_) return;
    queue_->state->owner = 0;
    queue_->state->cb = nullptr;
    if (!persistent_) {
      queue_->state->abandoned = true;
      queue_->surface->Abandon();
    } else {
      queue_->surface->SetListener(nullptr);
    }
  }

 private:
  std::shared_ptr<InputQueue> queue_;
  const uint64_t owner_;
  const bool persistent_;
};

class PersistentQueue final : public PersistentInputSurface {
 public:
  PersistentQueue() : queue_(std::make_shared<InputQueue>(1, 1)) {}
  explicit PersistentQueue(NativePersistentProducer* producer)
      : queue_(std::make_shared<InputQueue>(producer)) {}

  ANativeWindow* NativeWindow() const override { return queue_->surface.get(); }

  jobject NewJavaSurface(JNIEnv* env) override { return queue_->NewJavaSurface(env); }

  std::unique_ptr<InputSurface> Attach(int width, int height) override {
    std::lock_guard<std::recursive_mutex> lock(queue_->state->mu);
    if (width <= 0 || height <= 0 || queue_->state->abandoned ||
        queue_->state->owner != 0 || queue_->state->next_owner == 0) return nullptr;
    // Never recycle tokens, including after unsigned wrap: stale leases retain
    // their own generation until destruction. No per-frame ownership allocation.
    const uint64_t owner = queue_->state->next_owner++;
    queue_->surface->SetDefaultSize(width, height);
    queue_->state->owner = owner;
    queue_->Listen(owner);
    return std::make_unique<QueueInputSurface>(queue_, owner, true);
  }

 private:
  std::shared_ptr<InputQueue> queue_;
};

}  // namespace

InputSurface* CreateInputSurface(int width, int height) {
  auto queue = std::make_shared<InputQueue>(width > 0 ? width : 1, height > 0 ? height : 1);
  queue->state->owner = queue->state->next_owner++;
  queue->Listen(1);
  return new QueueInputSurface(std::move(queue), 1, false);
}

std::shared_ptr<PersistentInputSurface> CreatePersistentInputSurface(bool native_owned) {
  if (!native_owned) return std::make_shared<PersistentQueue>();
  android::sp<NativePersistentProducer> producer(new NativePersistentProducer());
  return std::make_shared<PersistentQueue>(producer.get());
}

std::shared_ptr<PersistentInputSurface> PersistentInputSurfaceFromWindow(ANativeWindow* window) {
  auto* producer = dynamic_cast<NativePersistentProducer*>(QueueSurface::From(window));
  if (producer == nullptr) return nullptr;
  return std::make_shared<PersistentQueue>(producer);
}

}  // namespace gesher::media
