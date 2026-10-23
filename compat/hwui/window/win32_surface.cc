// Gesher: an ANativeWindow over a Win32 window. See win32_surface.h.
#include "win32_surface.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <system/window.h>

#include "gesher_window.h"

namespace gesher {

// The surface's one buffer: pixels in system memory, `stride` pixels a row.
class Win32Surface::Buffer
    : public android::ANativeObjectBase<ANativeWindowBuffer, Win32Surface::Buffer,
                                        android::RefBase> {
 public:
  Buffer(int w, int h, int f) : pixels(static_cast<size_t>(w) * h, 0xff000000u) {
    width = w;
    height = h;
    stride = w;
    format = f;
    usage_deprecated = 0;
    layerCount = 1;
    reserved[0] = pixels.data();
    handle = nullptr;
    usage = 0;
  }

  std::vector<uint32_t> pixels;
};

namespace {

int BytesPerPixel(int format) {
  switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return 4;
    default:
      return 0;
  }
}

}  // namespace

Win32Surface::Win32Surface(Window* window) : android::Surface(nullptr), window_(window) {
  // hostgraphics' Surface sets perform, dequeueBuffer and query; the rest of
  // ANativeWindow's hooks are this class's.
  ANativeWindow::setSwapInterval = HookSetSwapInterval;
  ANativeWindow::queueBuffer = HookQueueBuffer;
  ANativeWindow::cancelBuffer = HookCancelBuffer;
  ANativeWindow::dequeueBuffer_DEPRECATED = nullptr;
  ANativeWindow::lockBuffer_DEPRECATED = nullptr;
  ANativeWindow::queueBuffer_DEPRECATED = nullptr;
  ANativeWindow::cancelBuffer_DEPRECATED = nullptr;
  const_cast<float&>(ANativeWindow::xdpi) = 160;
  const_cast<float&>(ANativeWindow::ydpi) = 160;
  const_cast<int&>(ANativeWindow::minSwapInterval) = 0;
  const_cast<int&>(ANativeWindow::maxSwapInterval) = 1;
}

Win32Surface::~Win32Surface() {
  delete window_;
}

Win32Surface* Win32Surface::From(ANativeWindow* window) {
  // Only a Win32Surface has this queueBuffer hook.
  if (window == nullptr || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC ||
      window->queueBuffer != HookQueueBuffer) {
    return nullptr;
  }
  return static_cast<Win32Surface*>(getSelf(window));
}

void Win32Surface::Size(int* width, int* height) const {
  if (user_width_ > 0 && user_height_ > 0) {
    *width = user_width_;
    *height = user_height_;
  } else {
    *width = window_->width();
    *height = window_->height();
  }
}

void Win32Surface::EnsureBuffer() {
  int w, h;
  Size(&w, &h);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (buffer_ == nullptr || buffer_->width != w || buffer_->height != h) {
    android::sp<Buffer> fresh = android::sp<Buffer>::make(w, h, format_);
    if (buffer_ != nullptr) {
      // Keep what overlaps: the next frame may redraw only its dirty part.
      for (int y = 0; y < h && y < buffer_->height; y++) {
        memcpy(&fresh->pixels[static_cast<size_t>(y) * w],
               &buffer_->pixels[static_cast<size_t>(y) * buffer_->stride],
               static_cast<size_t>(w < buffer_->width ? w : buffer_->width) * 4);
      }
    }
    buffer_ = fresh;
  }
  buffer_->format = format_;
}

int Win32Surface::lock(ANativeWindow_Buffer* out, ARect* dirty) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (locked_) return -EINVAL;
  int old_w = buffer_ != nullptr ? buffer_->width : 0;
  int old_h = buffer_ != nullptr ? buffer_->height : 0;
  EnsureBuffer();
  locked_ = true;
  out->width = buffer_->width;
  out->height = buffer_->height;
  out->stride = buffer_->stride;
  out->format = buffer_->format;
  out->bits = buffer_->pixels.data();
  memset(out->reserved, 0, sizeof out->reserved);
  if (dirty != nullptr) {
    // A new size invalidates everything; otherwise the buffer still holds the
    // last frame and only the dirty part needs drawing (an empty rectangle
    // means all of it, as in libgui).
    bool empty = dirty->left >= dirty->right || dirty->top >= dirty->bottom;
    if (empty || old_w != buffer_->width || old_h != buffer_->height) {
      *dirty = {0, 0, buffer_->width, buffer_->height};
    }
  }
  return 0;
}

int Win32Surface::unlockAndPost() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!locked_) return -EINVAL;
    locked_ = false;
  }
  Present();
  return 0;
}

void Win32Surface::Present() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (buffer_ == nullptr) return;
  int w, h, stride;
  uint8_t* dst = window_->Lock(&w, &h, &stride);
  const bool swap = buffer_->format != HAL_PIXEL_FORMAT_BGRA_8888;
  const bool opaque = buffer_->format == HAL_PIXEL_FORMAT_RGBX_8888;
  int rows = h < buffer_->height ? h : buffer_->height;
  int cols = w < buffer_->width ? w : buffer_->width;
  for (int y = 0; y < rows; y++) {
    const uint32_t* s = &buffer_->pixels[static_cast<size_t>(y) * buffer_->stride];
    uint32_t* d = reinterpret_cast<uint32_t*>(dst) + static_cast<size_t>(y) * stride;
    if (!swap) {
      memcpy(d, s, static_cast<size_t>(cols) * 4);
      continue;
    }
    // RGBA bytes (R in the low byte) to GDI's BGRA (B in the low byte).
    for (int x = 0; x < cols; x++) {
      uint32_t p = s[x];
      uint32_t q = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
      d[x] = opaque ? (q | 0xff000000u) : q;
    }
  }
  window_->UnlockAndPost();
  frames_++;
}

int Win32Surface::dequeueBuffer(ANativeWindowBuffer** buffer, int* fence_fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureBuffer();
  *buffer = buffer_.get();
  if (fence_fd != nullptr) *fence_fd = -1;
  return 0;
}

int Win32Surface::cancelBuffer(ANativeWindowBuffer*, int) {
  return 0;
}

int Win32Surface::queueBuffer(ANativeWindowBuffer*, int) {
  Present();
  return 0;
}

int Win32Surface::setSwapInterval(int) {
  return 0;
}

int Win32Surface::query(int what, int* value) const {
  int w, h;
  switch (what) {
    case NATIVE_WINDOW_WIDTH:
    case NATIVE_WINDOW_DEFAULT_WIDTH:
      Size(&w, &h);
      *value = w;
      return 0;
    case NATIVE_WINDOW_HEIGHT:
    case NATIVE_WINDOW_DEFAULT_HEIGHT:
      Size(&w, &h);
      *value = h;
      return 0;
    case NATIVE_WINDOW_FORMAT: {
      std::lock_guard<std::mutex> lock(mutex_);
      *value = format_;
      return 0;
    }
    case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
      *value = 1;
      return 0;
    case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER:
      *value = 1;
      return 0;
    case NATIVE_WINDOW_CONCRETE_TYPE:
      *value = NATIVE_WINDOW_SURFACE;
      return 0;
    case NATIVE_WINDOW_IS_VALID:
      *value = 1;
      return 0;
    case NATIVE_WINDOW_MAX_BUFFER_COUNT:
      *value = 1;
      return 0;
    default:
      // Transform hints, consumer state, buffer age, data space, ...: none.
      *value = 0;
      return 0;
  }
}

int Win32Surface::perform(int operation, va_list args) {
  switch (operation) {
    case NATIVE_WINDOW_LOCK: {
      auto* out = va_arg(args, ANativeWindow_Buffer*);
      auto* dirty = va_arg(args, ARect*);
      return lock(out, dirty);
    }
    case NATIVE_WINDOW_UNLOCK_AND_POST:
      return unlockAndPost();
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT: {
      int format = va_arg(args, int);
      if (format == 0) format = HAL_PIXEL_FORMAT_RGBA_8888;  // the default
      if (BytesPerPixel(format) == 0) return -EINVAL;
      std::lock_guard<std::mutex> lock(mutex_);
      format_ = format;
      return 0;
    }
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS: {
      int w = va_arg(args, int);
      int h = va_arg(args, int);
      std::lock_guard<std::mutex> lock(mutex_);
      user_width_ = w;
      user_height_ = h;
      return 0;
    }
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY: {
      int w = va_arg(args, int);
      int h = va_arg(args, int);
      int format = va_arg(args, int);
      std::lock_guard<std::mutex> lock(mutex_);
      user_width_ = w;
      user_height_ = h;
      if (format != 0 && BytesPerPixel(format) != 0) format_ = format;
      return 0;
    }
    case NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION:
      *va_arg(args, int64_t*) = 16666667;  // 60 Hz
      return 0;
    case NATIVE_WINDOW_GET_NEXT_FRAME_ID:
      *va_arg(args, uint64_t*) = frames_ + 1;
      return 0;
    case NATIVE_WINDOW_GET_CONSUMER_USAGE64:
      *va_arg(args, uint64_t*) = 0;
      return 0;
    case NATIVE_WINDOW_GET_LAST_DEQUEUE_START:
    case NATIVE_WINDOW_GET_LAST_DEQUEUE_DURATION:
    case NATIVE_WINDOW_GET_LAST_QUEUE_DURATION:
      *va_arg(args, int64_t*) = 0;
      return 0;
    case NATIVE_WINDOW_GET_COMPOSITOR_TIMING:
    case NATIVE_WINDOW_GET_FRAME_TIMESTAMPS:
    case NATIVE_WINDOW_GET_LAST_QUEUED_BUFFER:
    case NATIVE_WINDOW_GET_LAST_QUEUED_BUFFER2:
      return -ENOSYS;
    default:
      // Connections, crop, transforms, timestamps, data space, interceptors,
      // frame rate: accepted, and nothing to do without a compositor.
      return 0;
  }
}

int Win32Surface::HookSetSwapInterval(ANativeWindow* window, int interval) {
  return static_cast<Win32Surface*>(getSelf(window))->setSwapInterval(interval);
}

int Win32Surface::HookQueueBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer,
                                  int fence_fd) {
  return static_cast<Win32Surface*>(getSelf(window))->queueBuffer(buffer, fence_fd);
}

int Win32Surface::HookCancelBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer,
                                   int fence_fd) {
  return static_cast<Win32Surface*>(getSelf(window))->cancelBuffer(buffer, fence_fd);
}

}  // namespace gesher
