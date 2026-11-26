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
#include <optional>
#include <vector>

#include <utils/Timers.h>  // nsecs_t, which hostgraphics' Surface.h uses unincluded
#include <gui/Surface.h>
#include <ui/ANativeObjectBase.h>
#include <system/window.h>

namespace gesher::media {

struct VideoFrame;

struct SurfaceFrame {
  int width = 0, height = 0;  // stride == width
  std::vector<uint32_t> pixels;  // RGBA_8888 (R in the low byte)
  int64_t timestamp_ns = 0;
  uint64_t number = 0;
  int32_t dataspace = 0;  // HAL_DATASPACE_UNKNOWN until a producer tags the frame.
  int video_scaling_mode = 1;  // MediaCodec scale-to-fit; pixels remain uncropped.
};

class QueueSurface : public android::Surface {
 public:
  QueueSurface(int default_width, int default_height, bool single_buffer_mode = false);

  // The QueueSurface `window` is, or null.
  static QueueSurface* From(ANativeWindow* window);

  // The size buffers have when the producer sets none (SurfaceTexture's
  // setDefaultBufferSize, the SurfaceView's size).
  void SetDefaultSize(int width, int height);
  // Called on the producer's thread after each frame.
  void SetListener(std::function<void()> listener);
  // A forwarding producer waits for the remote consumer's publication result.
  void SetForwardingListener(std::function<int(const SurfaceFrame&)> listener);
  // Whether NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER reports 1: the surface's frames
  // are composited into a window (a SurfaceView's, as on Android) rather than
  // consumed by the app (an ImageReader's, a SurfaceTexture's: the default, 0).
  void SetQueuesToWindowComposer(bool queues) { queues_to_composer_ = queues; }
  // The last frame queued, or null.
  std::shared_ptr<const SurfaceFrame> Latest() const;
  // Return the single buffer only when it is still the GL consumer's released frame.
  void ReleaseFrame(uint64_t frame_number);
  uint64_t frames() const;
  // No producer may draw any more (SurfaceTexture.release).
  void Abandon();
  // A frame from a producer in another process (a Surface parceled to it,
  // compat/framework/surface_parcel.cc), queued as queueBuffer queues one:
  // `pixels` (RGBA_8888, `stride` pixels a row) become the latest frame, with
  // the producer's timestamp and dataspace. The listener is called. The
  // surface's own producer state is left alone. Returns -EAGAIN for an occupied
  // single buffer, -ENODEV after abandonment, or the publication status.
  int PostFrame(int width, int height, int stride, const uint32_t* pixels, int64_t timestamp_ns,
                int video_scaling_mode = 1, int32_t dataspace = 0);

  // A YUV consumer can accept decoded I420 without converting it to RGBA.
  // A missing result asks the producer to use the ordinary window path.
  virtual std::optional<int> PostVideoFrame(const VideoFrame&, int64_t) {
    return std::nullopt;
  }

  int lock(ANativeWindow_Buffer* out_buffer, ARect* in_out_dirty) override;
  int unlockAndPost() override;
  // Set a timestamp for this frame alone, preserving the native producer setting.
  int UnlockAndPostWithTimestamp(int64_t timestamp_ns, int video_scaling_mode = 1);
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
  class FramePool;
  class RecycledFrame;
  struct Publication {
    std::function<void()> listener;
    std::function<int(const SurfaceFrame&)> forwarding_listener;
    std::shared_ptr<const SurfaceFrame> frame;

    int Notify() const {
      if (listener) listener();
      return forwarding_listener ? forwarding_listener(*frame) : 0;
    }
  };

  void Size(int* width, int* height) const;
  void UpdateFrameGeometry();  // with mutex_ held; drops obsolete reusable pixels
  void EnsureBuffer();  // with mutex_ held
  bool SingleBufferOccupied() const;  // with mutex_ held
  int UnlockAndPost(std::optional<int64_t> timestamp_ns, int video_scaling_mode = 1);
  // Copies buffer_ as the latest frame with mutex_ held. Notify after unlocking.
  Publication PublishLocked(std::optional<int64_t> timestamp_ns, int video_scaling_mode = 1);

  static int HookSetSwapInterval(ANativeWindow* window, int interval);
  static int HookQueueBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd);
  static int HookCancelBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd);

  mutable std::mutex mutex_;
  android::sp<Buffer> buffer_;
  std::vector<android::sp<Buffer>> dequeued_;
  int format_ = 1;  // PIXEL_FORMAT_RGBA_8888
  int requested_width_ = 0, requested_height_ = 0;
  int user_width_ = 0, user_height_ = 0;
  int default_width_ = 1, default_height_ = 1;
  int64_t timestamp_ns_ = NATIVE_WINDOW_TIMESTAMP_AUTO;
  int32_t dataspace_ = 0;  // HAL_DATASPACE_UNKNOWN
  bool locked_ = false;
  bool buffer_contents_valid_ = false;
  bool abandoned_ = false;
  bool queues_to_composer_ = false;
  const bool single_buffer_mode_;
  std::shared_ptr<FramePool> frame_pool_;
  std::shared_ptr<const SurfaceFrame> latest_;
  uint64_t frames_ = 0;
  std::function<void()> listener_;
  std::function<int(const SurfaceFrame&)> forwarding_listener_;
};

}  // namespace gesher::media
