// Gesher: an ANativeWindow over a Win32 window (docs/graphics.md, "Window").
//
// Win32Surface is hostgraphics' android::Surface (the host form of
// libgui's), so android.view.Surface's natives, hwui and the NDK's
// ANativeWindow_* functions (native_window.cc) take it as they take a
// device's Surface. It owns one buffer, in Android's byte order
// (PIXEL_FORMAT_RGBA_8888 unless set otherwise): lock() hands it out,
// unlockAndPost() and queueBuffer() present it, converting to the window's
// GDI order (BGRA) under the window's lock. The buffer keeps its contents
// between frames, so a partial redraw (a dirty rectangle) is enough;
// dequeueBuffer() hands out the same buffer (reserved[0] = its pixels), which
// hwui's CPU pipeline (SkiaCpuPipeline) draws into.
#pragma once

#include <stdint.h>

#include <mutex>
#include <vector>

#include <gui/Surface.h>
#include <ui/ANativeObjectBase.h>

namespace gesher {

class Window;

class Win32Surface : public android::Surface {
 public:
  // Takes `window` over: the window lives as long as the surface (its last
  // reference: a Java Surface, hwui's renderer, an NDK client).
  explicit Win32Surface(Window* window);

  // The Win32Surface `window` is, or null when it is another ANativeWindow.
  static Win32Surface* From(ANativeWindow* window);

  Window* window() const { return window_; }

  int lock(ANativeWindow_Buffer* out_buffer, ARect* in_out_dirty) override;
  int unlockAndPost() override;
  int query(int what, int* value) const override;

  // Copies the buffer to the window and has it repaint.
  void Present();

  // Frames presented so far.
  uint64_t frames() const { return frames_; }

 protected:
  ~Win32Surface() override;

  int dequeueBuffer(ANativeWindowBuffer** buffer, int* fence_fd) override;
  int cancelBuffer(ANativeWindowBuffer* buffer, int fence_fd) override;
  int queueBuffer(ANativeWindowBuffer* buffer, int fence_fd) override;
  int perform(int operation, va_list args) override;
  int setSwapInterval(int interval) override;

 private:
  class Buffer;

  // The size buffers have now: the user dimensions when set, else the window's.
  void Size(int* width, int* height) const;
  // With mutex_ held: a buffer of the current size and format.
  void EnsureBuffer();

  static int HookSetSwapInterval(ANativeWindow* window, int interval);
  static int HookQueueBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd);
  static int HookCancelBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd);

  Window* window_;
  mutable std::mutex mutex_;
  android::sp<Buffer> buffer_;
  int format_ = 1;  // PIXEL_FORMAT_RGBA_8888
  int user_width_ = 0, user_height_ = 0;
  bool locked_ = false;
  uint64_t frames_ = 0;
};

}  // namespace gesher
