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
#include <memory>

namespace gesher::media {

// RGBA_8888 pixels (R in the low byte), width x height with stride == width.
using InputFrameCallback =
    std::function<void(const uint32_t* rgba, int width, int height, int64_t timestamp_ns)>;

class InputSurface {
 public:
  virtual ~InputSurface() = default;
  // A new android.view.Surface (a local reference) over this input.
  virtual jobject NewJavaSurface(JNIEnv* env) = 0;
  // Replace after any current callback on another thread has returned. A
  // callback may replace itself; its current invocation still owns its captures.
  virtual void SetCallback(InputFrameCallback cb) = 0;
  // Frames queued so far.
  virtual uint64_t frames() const = 0;
  // Whether this is a consumer lease on a separately owned persistent source.
  virtual bool persistent() const { return false; }
  // Retire this consumer. An ordinary input also abandons the producer; a
  // persistent lease leaves it usable by the next consumer. Waits for any
  // callback on another thread;
  // a callback may abandon its own input and then finish its current invocation.
  // Callback replacement after abandonment cannot reactivate the input.
  virtual void Abandon() = 0;
};

// A width x height input (hwui's side of the build: input_surface.cc).
InputSurface* CreateInputSurface(int width, int height);

// A producer owned by MediaCodec.PersistentSurface, independent of an encoder.
// Attach returns an exclusive consumer lease, or null while another lease is
// active. Retiring a lease quiesces its callbacks before allowing the next one.
// A lease retains the producer even if the application's Surface is released.
class PersistentInputSurface {
 public:
  virtual ~PersistentInputSurface() = default;
  virtual jobject NewJavaSurface(JNIEnv* env) = 0;
  virtual std::unique_ptr<InputSurface> Attach(int width, int height) = 0;
};

std::shared_ptr<PersistentInputSurface> CreatePersistentInputSurface();

}  // namespace gesher::media
