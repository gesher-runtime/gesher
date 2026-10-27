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
      InputFrameCallback cb;
      {
        std::lock_guard<std::mutex> lock(state->mu);
        cb = state->cb;
      }
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
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->cb = std::move(cb);
  }

  uint64_t frames() const override { return surface_->frames(); }

  void Abandon() override {
    SetCallback(nullptr);
    surface_->Abandon();
  }

 private:
  struct State {
    std::mutex mu;
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
