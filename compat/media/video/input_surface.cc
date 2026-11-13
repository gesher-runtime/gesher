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

class QueueInputSurface : public InputSurface {
 public:
  QueueInputSurface(int width, int height) : surface_(new QueueSurface(width, height)) {
    // The listener runs on the producer's thread after each frame.
    QueueSurface* raw = surface_.get();
    std::shared_ptr<State> state = state_;
    surface_->SetListener([raw, state] {
      // Keep admission and the invocation in the same critical section. The
      // consumers capture their own `this`, so clearing the stored callback
      // alone must not let Abandon return while a copied callback is running.
      // QueueSurface calls us after dropping its queue mutex.
      std::lock_guard<std::recursive_mutex> lock(state->mu);
      InputFrameCallback cb = state->cb;
      if (!cb) return;
      std::shared_ptr<const SurfaceFrame> f = raw->Latest();
      if (f == nullptr || f->pixels.empty()) return;
      cb(f->pixels.data(), f->width, f->height, f->timestamp_ns);
    });
  }

  ~QueueInputSurface() override { Abandon(); }

  jobject NewJavaSurface(JNIEnv* env) override {
    android::sp<android::Surface> s = surface_;
    return android::android_view_Surface_createFromSurface(env, s);
  }

  void SetCallback(InputFrameCallback cb) override {
    std::lock_guard<std::recursive_mutex> lock(state_->mu);
    if (!state_->abandoned) state_->cb = std::move(cb);
  }

  uint64_t frames() const override { return surface_->frames(); }

  void Abandon() override {
    std::lock_guard<std::recursive_mutex> lock(state_->mu);
    state_->abandoned = true;
    state_->cb = nullptr;
    surface_->Abandon();
  }

 private:
  struct State {
    // A callback may replace itself or abandon its input. A recursive mutex
    // permits that on the producer thread while still waiting for callbacks
    // on every other thread. The local callback copy keeps its captures alive.
    std::recursive_mutex mu;
    bool abandoned = false;
    InputFrameCallback cb;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
  android::sp<QueueSurface> surface_;
};

}  // namespace

InputSurface* CreateInputSurface(int width, int height) {
  return new QueueInputSurface(width > 0 ? width : 1, height > 0 ? height : 1);
}

}  // namespace gesher::media
