// Gesher media: a Surface whose frames stay in the process (docs/media.md
// "Video", ADR 0051): what a SurfaceTexture (TextureView) and a SurfaceView
// hand their producer (MediaCodec, MediaPlayer, an app's Canvas or NDK code).
//
// QueueSurface is hostgraphics' android::Surface, like the window's
// Win32Surface, but its buffers go nowhere on screen by themselves: each
// frame queued (queueBuffer, unlockAndPost) becomes its latest frame, an
// immutable RGBA copy, and its listener is called; hwui draws the latest
// frame where the TextureView or the SurfaceView is (surface_jni.cc). There is
// no compositor: the window redraws to show a new frame.
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <utils/Timers.h>  // nsecs_t, which hostgraphics' Surface.h uses unincluded
#include <gui/Surface.h>
#include <ui/ANativeObjectBase.h>

namespace gesher::media {

struct SurfaceFrame {
  int width = 0, height = 0;  // stride == width
  std::vector<uint32_t> pixels;  // RGBA_8888 (R in the low byte)
  int64_t timestamp_ns = 0;
  uint64_t number = 0;
};

class QueueSurface : public android::Surface {
 public:
  QueueSurface(int default_width, int default_height);

  // The QueueSurface `window` is, or null.
  static QueueSurface* From(ANativeWindow* window);

  // The size buffers have when the producer sets none (SurfaceTexture's
  // setDefaultBufferSize, the SurfaceView's size).
  void SetDefaultSize(int width, int height);
  // Called on the producer's thread after each frame.
  void SetListener(std::function<void()> listener);
  // The last frame queued, or null.
  std::shared_ptr<const SurfaceFrame> Latest() const;
  uint64_t frames() const;
  // No producer may draw any more (SurfaceTexture.release).
  void Abandon();

  int lock(ANativeWindow_Buffer* out_buffer, ARect* in_out_dirty) override;
  int unlockAndPost() override;
  int query(int what, int* value) const override;

 protected:
  ~QueueSurface() override;

  int dequeueBuffer(ANativeWindowBuffer** buffer, int* fence_fd) override;
  int cancelBuffer(ANativeWindowBuffer* buffer, int fence_fd) override;
  int queueBuffer(ANativeWindowBuffer* buffer, int fence_fd) override;
  int perform(int operation, va_list args) override;
  int setSwapInterval(int interval) override;

 private:
  class Buffer;

  void Size(int* width, int* height) const;
  void EnsureBuffer();  // with mutex_ held
  void Publish();       // copies buffer_ as the latest frame (mutex_ not held)

  static int HookSetSwapInterval(ANativeWindow* window, int interval);
  static int HookQueueBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd);
  static int HookCancelBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd);

  mutable std::mutex mutex_;
  android::sp<Buffer> buffer_;
  std::vector<android::sp<Buffer>> dequeued_;
  int format_ = 1;  // PIXEL_FORMAT_RGBA_8888
  int user_width_ = 0, user_height_ = 0;
  int default_width_ = 1, default_height_ = 1;
  int64_t timestamp_ns_ = 0;
  bool locked_ = false;
  bool abandoned_ = false;
  std::shared_ptr<const SurfaceFrame> latest_;
  uint64_t frames_ = 0;
  std::function<void()> listener_;
};

}  // namespace gesher::media
