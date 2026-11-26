// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <android/hardware_buffer.h>
#include <pthread.h>
#include <unistd.h>
#include <media/NdkImageReader.h>

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/native_fence.h>
#include <gesher/rgba_buffers.h>
#include <gesher/yuv_buffers.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>

#include <memory>
#include <mutex>
#include <new>
#include <unordered_set>
#include <utility>

#include "image_reader.h"

#include "abi_types.h"
#include "../surface/queue_surface.h"
#include "../video/video_decoder.h"

namespace {
namespace ahb = gesher::bionic::ahb;
using gesher::media::QueueSurface;
using gesher::media::SurfaceFrame;
using gesher::media::VideoFrame;

constexpr uint64_t kDefaultUsage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
constexpr int32_t kMaxImages = 64;
constexpr size_t kPendingFrames = 3;
constexpr int32_t kPixelStride = 4;

bool ValidSize(int32_t width, int32_t height) {
  return width > 0 && height > 0 &&
         static_cast<int64_t>(width) * height <= INT32_MAX / kPixelStride;
}

struct BufferDeleter {
  void operator()(AHardwareBuffer* storage) const { ahb::ReleaseImageBuffer(storage); }
};

struct ImageFrame {
  std::unique_ptr<AHardwareBuffer, BufferDeleter> storage;
  AHardwareBuffer_Planes planes{};
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = AIMAGE_FORMAT_RGBA_8888;
  int64_t timestamp_ns = 0;
};

using ImageCallback = void (BIONIC_SYSV*)(void*, AImageReader*);
using BufferRemovedCallback = void (BIONIC_SYSV*)(void*, AImageReader*, AHardwareBuffer*);

struct ReaderEvent {
  // A null buffer denotes an image-available notification.
  std::unique_ptr<AHardwareBuffer, BufferDeleter> removed_buffer;
};

struct ReaderState {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::unique_ptr<ImageFrame>> pending;
  std::unordered_set<AImage*> acquired;
  uint64_t next_frame = 1;
  std::deque<ReaderEvent> events;
  size_t image_notifications = 0;
  ImageCallback callback = nullptr;
  void* context = nullptr;
  BufferRemovedCallback removed_callback = nullptr;
  void* removed_context = nullptr;
  bool closed = false;
};

// The RGBA producer supports native window CPU writes. The YUV producer takes
// typed frames directly from camera and codec output. Neither exposes raw GPU
// buffer dequeue because those slots require graphics handles and fence reuse.
class ReaderWindow final : public QueueSurface {
 public:
  ReaderWindow(int32_t width, int32_t height, int32_t format, uint64_t usage,
               const std::shared_ptr<ReaderState>& state)
      : QueueSurface(width, height), width(width), height(height), format(format), usage(usage), state(state) {}

  int lock(ANativeWindow_Buffer* buffer, ARect* dirty) override {
    return format == AIMAGE_FORMAT_RGBA_8888 ? QueueSurface::lock(buffer, dirty) : -ENOSYS;
  }

  int query(int what, int* value) const override {
    if (what == NATIVE_WINDOW_FORMAT) {
      *value = format;
      return 0;
    }
    return QueueSurface::query(what, value);
  }

  std::optional<int> PostVideoFrame(const VideoFrame& frame, int64_t timestamp_ns) override;

  const int32_t width;
  const int32_t height;
  const int32_t format;
  const uint64_t usage;
  const std::weak_ptr<ReaderState> state;

 protected:
  int dequeueBuffer(ANativeWindowBuffer**, int*) override { return -ENOSYS; }

  int queueBuffer(ANativeWindowBuffer*, int fence_fd) override {
    if (fence_fd >= 0) close(fence_fd);
    return -ENOSYS;
  }

  int cancelBuffer(ANativeWindowBuffer*, int fence_fd) override {
    if (fence_fd >= 0) close(fence_fd);
    return -ENOSYS;
  }

  int perform(int operation, va_list args) override {
    va_list probe;
    va_copy(probe, args);
    bool supported = true;
    switch (operation) {
      case NATIVE_WINDOW_SET_BUFFERS_FORMAT: {
        const int format = va_arg(probe, int);
        supported = format == 0 || format == this->format;
        break;
      }
      case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
      case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
      case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY: {
        const int width = va_arg(probe, int);
        const int height = va_arg(probe, int);
        supported = (width == 0 && height == 0) || ValidSize(width, height);
        if (operation == NATIVE_WINDOW_SET_BUFFERS_GEOMETRY) {
          const int format = va_arg(probe, int);
          supported = supported && (format == 0 || format == this->format);
        }
        break;
      }
      case NATIVE_WINDOW_GET_CONSUMER_USAGE64:
        *va_arg(probe, uint64_t*) = usage;
        va_end(probe);
        return 0;
    }
    va_end(probe);
    return supported ? QueueSurface::perform(operation, args) : -EINVAL;
  }
};

// Caller holds the reader mutex. Keep the reader's reference through the
// callback so an app can identify the removed buffer or take a reference to it.
void NotifyBufferRemoved(ReaderState& state, ImageFrame& frame) {
  if (state.closed || state.removed_callback == nullptr) return;
  state.events.push_back({std::move(frame.storage)});
  state.changed.notify_all();
}

// Caller holds the reader mutex. The acquired images own separate buffers.
int Enqueue(ReaderState& state, std::unique_ptr<ImageFrame> frame) {
  // A stalled removal callback retains its buffer. Stop accepting frames while
  // too many callbacks are queued so a producer cannot grow that memory alone.
  if (state.events.size() >= 2 * kPendingFrames) return -EAGAIN;
  if (state.pending.size() == kPendingFrames) {
    NotifyBufferRemoved(state, *state.pending.front());
    state.pending.pop_front();
  }
  state.pending.push_back(std::move(frame));
  if (state.callback != nullptr && state.image_notifications < kPendingFrames) {
    state.events.emplace_back();
    ++state.image_notifications;
  }
  return 0;
}

int Publish(const std::weak_ptr<ReaderState>& weak, const SurfaceFrame& frame, uint64_t usage) {
  auto state = weak.lock();
  if (state == nullptr) return -ENODEV;

  const bool valid_size = ValidSize(frame.width, frame.height);
  std::unique_ptr<ImageFrame> image;
  if (valid_size) {
    auto* storage = static_cast<AHardwareBuffer*>(
        ahb::AllocateRgbaBuffer(frame.width, frame.height, false, usage));
    if (storage != nullptr) {
      image = std::make_unique<ImageFrame>();
      image->storage.reset(storage);
      auto* pixels = ahb::RgbaBufferPixels(storage);
      std::memcpy(pixels, frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
      ahb::MarkRgbaWritten(storage);
      image->planes.planeCount = 1;
      image->planes.planes[0] = {
          pixels, kPixelStride, static_cast<uint32_t>(frame.width * kPixelStride)};
      image->width = frame.width;
      image->height = frame.height;
      image->timestamp_ns = frame.timestamp_ns;
    }
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  // QueueSurface hands us each specific frame after releasing its producer
  // lock. A later producer can finish copying before an earlier one.
  state->changed.wait(lock, [&] { return state->closed || state->next_frame == frame.number; });
  if (state->closed) return -ENODEV;
  ++state->next_frame;
  state->changed.notify_all();
  if (!image) return valid_size ? -ENOMEM : -EINVAL;
  return Enqueue(*state, std::move(image));
}

ReaderWindow* YuvWindow(ANativeWindow* window) {
  auto* reader = dynamic_cast<ReaderWindow*>(QueueSurface::From(window));
  return reader != nullptr && reader->format == AIMAGE_FORMAT_YUV_420_888 ? reader : nullptr;
}

int32_t ChromaSize(int32_t extent) { return extent / 2 + extent % 2; }

std::unique_ptr<ImageFrame> NewYuvFrame(int width, int height, int64_t timestamp_ns, uint64_t usage) {
  auto image = std::make_unique<ImageFrame>();
  image->storage.reset(ahb::AllocateYuv420Buffer(width, height, usage, &image->planes));
  if (!image->storage) return nullptr;
  image->width = width;
  image->height = height;
  image->format = AIMAGE_FORMAT_YUV_420_888;
  image->timestamp_ns = timestamp_ns;
  return image;
}

int PublishYuv(ReaderWindow& reader, std::unique_ptr<ImageFrame> frame) {
  const auto state = reader.state.lock();
  if (state == nullptr) return -ENODEV;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed) return -ENODEV;
  const int status = Enqueue(*state, std::move(frame));
  if (status == 0) state->changed.notify_all();
  return status;
}
std::optional<int> ReaderWindow::PostVideoFrame(const VideoFrame& frame, int64_t timestamp_ns) {
  if (format != AIMAGE_FORMAT_YUV_420_888) return std::nullopt;

  // Validate the complete backing storage before forming cropped pointers.
  if (frame.width <= 0 || frame.height <= 0 ||
      frame.stride_y < frame.width || frame.stride_uv < ChromaSize(frame.width) ||
      frame.crop_left < 0 || frame.crop_top < 0 ||
      frame.crop_right < frame.crop_left || frame.crop_right >= frame.width ||
      frame.crop_bottom < frame.crop_top || frame.crop_bottom >= frame.height) {
    return -EINVAL;
  }
  const auto y_bytes = static_cast<uint64_t>(frame.stride_y) * frame.height;
  const auto c_bytes = static_cast<uint64_t>(frame.stride_uv) * ChromaSize(frame.height);
  if (y_bytes + 2 * c_bytes > frame.data.size()) return -EINVAL;

  const int width = frame.visible_width(), height = frame.visible_height();
  if (!ValidSize(width, height)) return -EINVAL;
  auto image = NewYuvFrame(width, height, timestamp_ns, usage);
  if (image == nullptr) return -ENOMEM;
  const size_t y_offset = static_cast<size_t>(frame.crop_top) * frame.stride_y + frame.crop_left;
  const size_t c_offset =
      static_cast<size_t>(frame.crop_top / 2) * frame.stride_uv + frame.crop_left / 2;
  const uint8_t* sources[] = {frame.data.data() + y_offset,
                              frame.data.data() + y_bytes + c_offset,
                              frame.data.data() + y_bytes + c_bytes + c_offset};
  const int strides[] = {frame.stride_y, frame.stride_uv, frame.stride_uv};
  for (int plane = 0; plane < 3; ++plane) {
    auto* target = static_cast<uint8_t*>(image->planes.planes[plane].data);
    const size_t row_bytes = image->planes.planes[plane].rowStride;
    const int rows = plane == 0 ? height : ChromaSize(height);
    for (int row = 0; row < rows; ++row) {
      std::memcpy(target + static_cast<size_t>(row) * row_bytes,
                  sources[plane] + static_cast<size_t>(row) * strides[plane], row_bytes);
    }
  }
  return PublishYuv(*this, std::move(image));
}
}  // namespace

namespace gesher::media {
std::optional<int> QueueNdkReaderCameraFrame(ANativeWindow* window, const uint8_t* nv12,
                                             size_t size, int width, int height,
                                             int64_t timestamp_ns) {
  auto* reader = YuvWindow(window);
  if (reader == nullptr) return std::nullopt;
  if (nv12 == nullptr || width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0)
    return -EINVAL;
  const uint64_t y_bytes = static_cast<uint64_t>(width) * height;
  if (size < y_bytes + y_bytes / 2) return -EINVAL;

  const int target_width = reader->width, target_height = reader->height;
  auto image = NewYuvFrame(target_width, target_height, timestamp_ns, reader->usage);
  if (image == nullptr) return -ENOMEM;
  const auto* uv = nv12 + y_bytes;
  for (int plane = 0; plane < 3; ++plane) {
    const bool chroma = plane != 0;
    const uint8_t* source = chroma ? uv + plane - 1 : nv12;
    auto* target = static_cast<uint8_t*>(image->planes.planes[plane].data);
    const int source_rows = chroma ? height / 2 : height;
    const int source_columns = chroma ? width / 2 : width;
    const int rows = chroma ? ChromaSize(target_height) : target_height;
    const int columns = chroma ? ChromaSize(target_width) : target_width;
    const int pixel_stride = chroma ? 2 : 1;
    if (!chroma && target_width == width && target_height == height) {
      std::memcpy(target, source, y_bytes);
      continue;
    }
    for (int row = 0; row < rows; ++row) {
      const size_t source_row = static_cast<int64_t>(row) * source_rows / rows * width;
      for (int column = 0; column < columns; ++column) {
        const size_t source_column = static_cast<int64_t>(column) * source_columns / columns;
        target[static_cast<size_t>(row) * columns + column] =
            source[source_row + source_column * pixel_stride];
      }
    }
  }
  return PublishYuv(*reader, std::move(image));
}
}  // namespace gesher::media

struct AImageReader {
  const int32_t width;
  const int32_t height;
  const int32_t max_images;
  const int32_t format;
  std::shared_ptr<ReaderState> state;
  android::sp<ReaderWindow> window;
  pthread_t worker{};
  bool has_worker = false;
};

struct AImage {
  std::shared_ptr<ReaderState> owner;
  std::unique_ptr<ImageFrame> frame;
};

namespace {
media_status_t BIONIC_SYSV NewWithUsage(int32_t width, int32_t height, int32_t format,
                                      uint64_t usage, int32_t max_images, AImageReader** out) {
  if (out == nullptr || !ValidSize(width, height) || max_images < 1 || max_images > kMaxImages ||
      (format != AIMAGE_FORMAT_RGBA_8888 && format != AIMAGE_FORMAT_YUV_420_888) ||
      (usage != kDefaultUsage && usage != AHARDWAREBUFFER_USAGE_CPU_READ_RARELY)) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  auto state = std::make_shared<ReaderState>();
  auto window = android::sp<ReaderWindow>::make(width, height, format, usage, state);
  window->SetForwardingListener(
      [weak = std::weak_ptr<ReaderState>(state), format, usage](const SurfaceFrame& frame) {
        return format == AIMAGE_FORMAT_RGBA_8888 ? Publish(weak, frame, usage) : -EINVAL;
      });
  *out = new (std::nothrow) AImageReader{width, height, max_images, format, std::move(state),
                                        std::move(window), {}, false};
  return *out != nullptr ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

media_status_t BIONIC_SYSV New(int32_t width, int32_t height, int32_t format, int32_t max_images,
                             AImageReader** out) {
  return NewWithUsage(width, height, format, kDefaultUsage, max_images, out);
}

void BIONIC_SYSV DeleteReader(AImageReader* reader) {
  if (reader == nullptr) return;
  const auto state = reader->state;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->pending.clear();
    state->events.clear();
    for (AImage* image : state->acquired) image->frame.reset();
    state->acquired.clear();
    state->changed.notify_all();
  }
  reader->window->Abandon();
  if (reader->has_worker) {
    // An image listener is allowed to delete its own reader.
    if (pthread_equal(reader->worker, pthread_self())) {
      pthread_detach(reader->worker);
    } else {
      pthread_join(reader->worker, nullptr);
    }
  }
  delete reader;
}

media_status_t BIONIC_SYSV GetWindow(AImageReader* reader, ANativeWindow** out) {
  if (reader == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = reader->window.get();
  return AMEDIA_OK;
}

template <const int32_t AImageReader::*Field>
media_status_t BIONIC_SYSV GetReaderField(const AImageReader* reader, int32_t* out) {
  if (reader == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = reader->*Field;
  return AMEDIA_OK;
}

media_status_t Acquire(AImageReader* reader, AImage** out, bool latest, int* fence_fd = nullptr) {
  if (reader == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  const auto state = reader->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed) return AMEDIA_ERROR_INVALID_OBJECT;
  const size_t remaining = static_cast<size_t>(reader->max_images) - state->acquired.size();
  if (remaining == 0) return AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED;
  if (state->pending.empty()) return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
  auto image = std::unique_ptr<AImage>(new (std::nothrow) AImage{state, {}});
  if (!image) return AMEDIA_ERROR_UNKNOWN;
  if (latest && remaining >= 2) {
    while (state->pending.size() > 1) {
      NotifyBufferRemoved(*state, *state->pending.front());
      state->pending.pop_front();
    }
  }
  image->frame = std::move(state->pending.front());
  state->pending.pop_front();
  state->acquired.insert(image.get());
  *out = image.release();
  // Publication copied the producer pixels before making this image ready.
  if (fence_fd != nullptr) *fence_fd = -1;
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV AcquireNext(AImageReader* reader, AImage** out) {
  return Acquire(reader, out, false);
}

media_status_t BIONIC_SYSV AcquireLatest(AImageReader* reader, AImage** out) {
  return Acquire(reader, out, true);
}

media_status_t BIONIC_SYSV AcquireNextAsync(AImageReader* reader, AImage** out, int* fence_fd) {
  return Acquire(reader, out, false, fence_fd);
}

media_status_t BIONIC_SYSV AcquireLatestAsync(AImageReader* reader, AImage** out, int* fence_fd) {
  return Acquire(reader, out, true, fence_fd);
}

void* RunListener(void* context) {
  auto* reader = static_cast<AImageReader*>(context);
  const auto state = reader->state;
  gesher::bionic::EnterLoadedCode();
  std::unique_lock<std::mutex> lock(state->mutex);
  for (;;) {
    state->changed.wait(lock, [&] { return state->closed || !state->events.empty(); });
    if (state->closed) return nullptr;
    auto event = std::move(state->events.front());
    state->events.pop_front();
    if (event.removed_buffer) {
      auto callback = state->removed_callback;
      void* context = state->removed_context;
      lock.unlock();
      callback(context, reader, event.removed_buffer.get());
      event.removed_buffer.reset();
    } else {
      --state->image_notifications;
      auto callback = state->callback;
      void* context = state->context;
      lock.unlock();
      callback(context, reader);
    }
    lock.lock();
  }
}

template <class Listener, class Callback>
media_status_t SetListener(AImageReader* reader, Listener* listener,
                           Callback Listener::* callback_field, bool buffer_removed) {
  if (reader == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  void* address = listener != nullptr ? reinterpret_cast<void*>(listener->*callback_field) : nullptr;
  if (address != nullptr && gesher::arm64::IsGuestCode(address)) {
    address = gesher::arm64::HostEntry(address);
    if (address == nullptr) return AMEDIA_ERROR_UNKNOWN;
  }

  const auto state = reader->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed) return AMEDIA_ERROR_INVALID_OBJECT;
  if (address != nullptr && !reader->has_worker) {
    if (pthread_create(&reader->worker, nullptr, RunListener, reader) != 0) {
      return AMEDIA_ERROR_UNKNOWN;
    }
    reader->has_worker = true;
  }
  void* context = listener != nullptr ? listener->context : nullptr;
  if (buffer_removed) {
    state->removed_callback = reinterpret_cast<BufferRemovedCallback>(address);
    state->removed_context = context;
  } else {
    state->callback = reinterpret_cast<ImageCallback>(address);
    state->context = context;
    state->image_notifications = 0;
  }
  std::erase_if(state->events, [buffer_removed](const ReaderEvent& event) {
    return static_cast<bool>(event.removed_buffer) == buffer_removed;
  });
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV SetImageListener(AImageReader* reader,
                                           AImageReader_ImageListener* listener) {
  return SetListener(reader, listener, &AImageReader_ImageListener::onImageAvailable, false);
}

media_status_t BIONIC_SYSV SetBufferRemovedListener(
    AImageReader* reader, AImageReader_BufferRemovedListener* listener) {
  return SetListener(reader, listener, &AImageReader_BufferRemovedListener::onBufferRemoved, true);
}

struct DeferredImageRelease {
  std::weak_ptr<ReaderState> owner;
  std::unique_ptr<ImageFrame> frame;
};

void ReleaseImageFrame(void* opaque) {
  std::unique_ptr<DeferredImageRelease> release(static_cast<DeferredImageRelease*>(opaque));
  if (auto owner = release->owner.lock()) {
    std::lock_guard<std::mutex> lock(owner->mutex);
    NotifyBufferRemoved(*owner, *release->frame);
  }
}

void BIONIC_SYSV DeleteImageAsync(AImage* image, int release_fence_fd) {
  if (image == nullptr) return;
  std::shared_ptr<gesher::NativeFence> fence;
  if (release_fence_fd >= 0) {
    // The caller transfers this descriptor. Retain the native event first,
    // since closing the fd drops the registry's own reference to it.
    fence = gesher::LookupNativeFence(release_fence_fd);
    close(release_fence_fd);
  }

  std::unique_ptr<ImageFrame> frame;
  {
    std::lock_guard<std::mutex> lock(image->owner->mutex);
    image->owner->acquired.erase(image);
    frame = std::move(image->frame);
    if (!fence && frame) NotifyBufferRemoved(*image->owner, *frame);
  }
  auto owner = std::weak_ptr<ReaderState>(image->owner);
  delete image;
  if (fence && frame) {
    gesher::ReleaseAfterNativeFence(fence, new DeferredImageRelease{std::move(owner), std::move(frame)},
                                    ReleaseImageFrame);
  }
}

void BIONIC_SYSV DeleteImage(AImage* image) { DeleteImageAsync(image, -1); }

template <class T, T ImageFrame::*Field>
media_status_t BIONIC_SYSV GetImageField(const AImage* image, T* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = image->frame.get()->*Field;
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV GetCropRect(const AImage* image, AImageCropRect* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = {0, 0, image->frame->width, image->frame->height};
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV GetPlaneCount(const AImage* image, int32_t* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = static_cast<int32_t>(image->frame->planes.planeCount);
  return AMEDIA_OK;
}

// Callers validate the output and hold the reader mutex while the image is used.
media_status_t CheckPlane(const AImage& image, int plane) {
  if (!image.frame) return AMEDIA_ERROR_INVALID_OBJECT;
  if (plane < 0 || static_cast<uint32_t>(plane) >= image.frame->planes.planeCount)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  return AMEDIA_OK;
}

template <uint32_t AHardwareBuffer_Plane::*Field>
media_status_t BIONIC_SYSV GetPlaneStride(const AImage* image, int plane, int32_t* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  const media_status_t status = CheckPlane(*image, plane);
  if (status == AMEDIA_OK) *out = static_cast<int32_t>(image->frame->planes.planes[plane].*Field);
  return status;
}

media_status_t BIONIC_SYSV GetPlaneData(const AImage* image, int plane, uint8_t** out, int* length) {
  if (image == nullptr || out == nullptr || length == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  const media_status_t status = CheckPlane(*image, plane);
  if (status != AMEDIA_OK) return status;
  const auto& layout = image->frame->planes.planes[plane];
  const int rows = plane == 0 ? image->frame->height : ChromaSize(image->frame->height);
  *out = static_cast<uint8_t*>(layout.data);
  *length = static_cast<int>(layout.rowStride * rows);
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV GetHardwareBuffer(const AImage* image, AHardwareBuffer** out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  // Borrowed for the lifetime of this image. An explicit AHardwareBuffer_acquire
  // keeps the pixel storage independently of the image and reader.
  *out = image->frame->storage.get();
  return AMEDIA_OK;
}

bool Register() {
  using gesher::bionic::AddBionicSymbol;
#define ENTRY(name, ...) AddBionicSymbol(#name, reinterpret_cast<void*>(&__VA_ARGS__))
  ENTRY(AImageReader_new, New);
  ENTRY(AImageReader_newWithUsage, NewWithUsage);
  ENTRY(AImageReader_delete, DeleteReader);
  ENTRY(AImageReader_getWindow, GetWindow);
  ENTRY(AImageReader_getWidth, GetReaderField<&AImageReader::width>);
  ENTRY(AImageReader_getHeight, GetReaderField<&AImageReader::height>);
  ENTRY(AImageReader_getMaxImages, GetReaderField<&AImageReader::max_images>);
  ENTRY(AImageReader_getFormat, GetReaderField<&AImageReader::format>);
  ENTRY(AImageReader_acquireNextImage, AcquireNext);
  ENTRY(AImageReader_acquireLatestImage, AcquireLatest);
  ENTRY(AImageReader_acquireNextImageAsync, AcquireNextAsync);
  ENTRY(AImageReader_acquireLatestImageAsync, AcquireLatestAsync);
  ENTRY(AImageReader_setImageListener, SetImageListener);
  ENTRY(AImageReader_setBufferRemovedListener, SetBufferRemovedListener);
  ENTRY(AImage_delete, DeleteImage);
  ENTRY(AImage_deleteAsync, DeleteImageAsync);
  ENTRY(AImage_getWidth, GetImageField<int32_t, &ImageFrame::width>);
  ENTRY(AImage_getHeight, GetImageField<int32_t, &ImageFrame::height>);
  ENTRY(AImage_getFormat, GetImageField<int32_t, &ImageFrame::format>);
  ENTRY(AImage_getTimestamp, GetImageField<int64_t, &ImageFrame::timestamp_ns>);
  ENTRY(AImage_getCropRect, GetCropRect);
  ENTRY(AImage_getNumberOfPlanes, GetPlaneCount);
  ENTRY(AImage_getPlanePixelStride, GetPlaneStride<&AHardwareBuffer_Plane::pixelStride>);
  ENTRY(AImage_getPlaneRowStride, GetPlaneStride<&AHardwareBuffer_Plane::rowStride>);
  ENTRY(AImage_getPlaneData, GetPlaneData);
  ENTRY(AImage_getHardwareBuffer, GetHardwareBuffer);
#undef ENTRY
  return true;
}

[[maybe_unused]] const bool g_registered = Register();
}  // namespace
