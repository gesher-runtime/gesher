// Gesher media: a Surface whose frames stay in the process. See queue_surface.h.
#include "queue_surface.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <system/window.h>

namespace gesher::media {

class QueueSurface::Buffer
    : public android::ANativeObjectBase<ANativeWindowBuffer, QueueSurface::Buffer, android::RefBase> {
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

QueueSurface::QueueSurface(int default_width, int default_height) : android::Surface(nullptr) {
  default_width_ = default_width > 0 ? default_width : 1;
  default_height_ = default_height > 0 ? default_height : 1;
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

QueueSurface::~QueueSurface() = default;

QueueSurface* QueueSurface::From(ANativeWindow* window) {
  if (window == nullptr || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC ||
      window->queueBuffer != HookQueueBuffer) {
    return nullptr;
  }
  return static_cast<QueueSurface*>(getSelf(window));
}

void QueueSurface::SetDefaultSize(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (width > 0) default_width_ = width;
  if (height > 0) default_height_ = height;
}

void QueueSurface::SetListener(std::function<void()> listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = std::move(listener);
}

std::shared_ptr<const SurfaceFrame> QueueSurface::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

uint64_t QueueSurface::frames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_;
}

void QueueSurface::Abandon() {
  std::lock_guard<std::mutex> lock(mutex_);
  abandoned_ = true;
  listener_ = nullptr;
}

void QueueSurface::Size(int* width, int* height) const {
  if (user_width_ > 0 && user_height_ > 0) {
    *width = user_width_;
    *height = user_height_;
  } else {
    *width = default_width_;
    *height = default_height_;
  }
}

void QueueSurface::EnsureBuffer() {
  int w, h;
  Size(&w, &h);
  if (buffer_ == nullptr || buffer_->width != w || buffer_->height != h) {
    buffer_ = android::sp<Buffer>::make(w, h, format_);
  }
  buffer_->format = format_;
}

int QueueSurface::lock(ANativeWindow_Buffer* out, ARect* dirty) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (locked_ || abandoned_) return -EINVAL;
  EnsureBuffer();
  locked_ = true;
  out->width = buffer_->width;
  out->height = buffer_->height;
  out->stride = buffer_->stride;
  out->format = buffer_->format;
  out->bits = buffer_->pixels.data();
  memset(out->reserved, 0, sizeof out->reserved);
  // The buffer keeps the last frame: only the dirty part needs drawing.
  if (dirty != nullptr && (dirty->left >= dirty->right || dirty->top >= dirty->bottom)) {
    *dirty = {0, 0, buffer_->width, buffer_->height};
  }
  return 0;
}

int QueueSurface::unlockAndPost() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!locked_) return -EINVAL;
    locked_ = false;
  }
  Publish();
  return 0;
}

void QueueSurface::Publish() {
  std::function<void()> listener;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_ == nullptr || abandoned_) return;
    auto f = std::make_shared<SurfaceFrame>();
    f->width = buffer_->width;
    f->height = buffer_->height;
    const bool opaque = buffer_->format == HAL_PIXEL_FORMAT_RGBX_8888;
    const bool bgra = buffer_->format == HAL_PIXEL_FORMAT_BGRA_8888;
    f->pixels.resize(static_cast<size_t>(f->width) * f->height);
    for (int y = 0; y < f->height; y++) {
      const uint32_t* s = &buffer_->pixels[static_cast<size_t>(y) * buffer_->stride];
      uint32_t* d = &f->pixels[static_cast<size_t>(y) * f->width];
      if (!opaque && !bgra) {
        memcpy(d, s, static_cast<size_t>(f->width) * 4);
        continue;
      }
      for (int x = 0; x < f->width; x++) {
        uint32_t p = s[x];
        if (bgra) p = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
        d[x] = opaque ? (p | 0xff000000u) : p;
      }
    }
    f->timestamp_ns = timestamp_ns_ != 0 ? timestamp_ns_ : systemTime(SYSTEM_TIME_MONOTONIC);
    f->number = ++frames_;
    timestamp_ns_ = 0;
    latest_ = std::move(f);
    listener = listener_;
  }
  if (listener) listener();
}

int QueueSurface::dequeueBuffer(ANativeWindowBuffer** buffer, int* fence_fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (abandoned_) return -ENODEV;
  EnsureBuffer();
  *buffer = buffer_.get();
  dequeued_.push_back(buffer_);
  if (fence_fd != nullptr) *fence_fd = -1;
  return 0;
}

int QueueSurface::cancelBuffer(ANativeWindowBuffer* buffer, int) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = dequeued_.begin(); it != dequeued_.end(); ++it) {
    if (static_cast<ANativeWindowBuffer*>(it->get()) == buffer) {
      dequeued_.erase(it);
      break;
    }
  }
  return 0;
}

int QueueSurface::queueBuffer(ANativeWindowBuffer* buffer, int) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = dequeued_.begin(); it != dequeued_.end(); ++it) {
      if (static_cast<ANativeWindowBuffer*>(it->get()) == buffer) {
        buffer_ = *it;
        dequeued_.erase(it);
        break;
      }
    }
  }
  Publish();
  return 0;
}

int QueueSurface::setSwapInterval(int) {
  return 0;
}

int QueueSurface::query(int what, int* value) const {
  std::lock_guard<std::mutex> lock(mutex_);
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
    case NATIVE_WINDOW_FORMAT:
      *value = format_;
      return 0;
    case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
      *value = 1;
      return 0;
    case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER:
      *value = 0;
      return 0;
    case NATIVE_WINDOW_CONCRETE_TYPE:
      *value = NATIVE_WINDOW_SURFACE;
      return 0;
    case NATIVE_WINDOW_IS_VALID:
      *value = abandoned_ ? 0 : 1;
      return 0;
    case NATIVE_WINDOW_MAX_BUFFER_COUNT:
      *value = 1;
      return 0;
    default:
      *value = 0;
      return 0;
  }
}

int QueueSurface::perform(int operation, va_list args) {
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
      if (format == 0) format = HAL_PIXEL_FORMAT_RGBA_8888;
      if (format != HAL_PIXEL_FORMAT_RGBA_8888 && format != HAL_PIXEL_FORMAT_RGBX_8888 &&
          format != HAL_PIXEL_FORMAT_BGRA_8888) {
        return -EINVAL;
      }
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
      if (format == HAL_PIXEL_FORMAT_RGBA_8888 || format == HAL_PIXEL_FORMAT_RGBX_8888 ||
          format == HAL_PIXEL_FORMAT_BGRA_8888) {
        format_ = format;
      }
      return 0;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP: {
      int64_t t = va_arg(args, int64_t);
      std::lock_guard<std::mutex> lock(mutex_);
      timestamp_ns_ = t;
      return 0;
    }
    case NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION:
      *va_arg(args, int64_t*) = 16666667;
      return 0;
    case NATIVE_WINDOW_GET_NEXT_FRAME_ID: {
      std::lock_guard<std::mutex> lock(mutex_);
      *va_arg(args, uint64_t*) = frames_ + 1;
      return 0;
    }
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
      return 0;
  }
}

int QueueSurface::HookSetSwapInterval(ANativeWindow* window, int interval) {
  return static_cast<QueueSurface*>(getSelf(window))->setSwapInterval(interval);
}

int QueueSurface::HookQueueBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer, int fence_fd) {
  return static_cast<QueueSurface*>(getSelf(window))->queueBuffer(buffer, fence_fd);
}

int QueueSurface::HookCancelBuffer(ANativeWindow* window, ANativeWindowBuffer* buffer,
                                   int fence_fd) {
  return static_cast<QueueSurface*>(getSelf(window))->cancelBuffer(buffer, fence_fd);
}

}  // namespace gesher::media
