// Gesher media: an input Surface (docs/media.md "Recording", ADR 0059): what
// MediaCodec.createInputSurface, MediaCodec.setInputSurface and
// MediaRecorder.getSurface give an app or a camera. Built into hwui (it is a
// QueueSurface, compat/media/surface/queue_surface.h, hostgraphics' Surface):
// every frame queued into it (Canvas through lockCanvas/unlockCanvasAndPost,
// ANativeWindow_lock, the camera service's frames) is handed, as RGBA, to the
// consumer's callback on the producer's thread.
#pragma once

#include <jni.h>

#include <cstdint>
#include <functional>

namespace gesher::media {

// RGBA_8888 pixels (R in the low byte), width x height with stride == width.
using InputFrameCallback =
    std::function<void(const uint32_t* rgba, int width, int height, int64_t timestamp_ns)>;

class InputSurface {
 public:
  virtual ~InputSurface() = default;
  // A new android.view.Surface (a local reference) over this input.
  virtual jobject NewJavaSurface(JNIEnv* env) = 0;
  virtual void SetCallback(InputFrameCallback cb) = 0;
  // Frames queued so far.
  virtual uint64_t frames() const = 0;
  // No producer may draw any more.
  virtual void Abandon() = 0;
};

// A width x height input (hwui's side of the build: input_surface.cc).
InputSurface* CreateInputSurface(int width, int height);

}  // namespace gesher::media
