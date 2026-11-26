// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <android/hardware_buffer.h>
#include <pthread.h>
#include <unistd.h>
#include <media/NdkImageReader.h>

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/rgba_buffers.h>

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>

#include <memory>
#include <mutex>
#include <new>
#include <unordered_set>

#include "abi_types.h"
#include "../surface/queue_surface.h"

namespace {
namespace ahb = gesher::bionic::ahb;
using gesher::media::QueueSurface;
using gesher::media::SurfaceFrame;

constexpr int32_t kFormat = AIMAGE_FORMAT_RGBA_8888;
constexpr uint64_t kUsage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
constexpr int32_t kMaxImages = 64;
constexpr size_t kPendingFrames = 3;
constexpr int32_t kPixelStride = 4;

bool ValidSize(int32_t width, int32_t height) {
  return width > 0 && height > 0 &&
         static_cast<int64_t>(width) * height <= INT32_MAX / kPixelStride;
}

struct BufferDeleter {
  void operator()(void* storage) const { ahb::ReleaseRgbaBuffer(storage); }
};

struct ImageFrame {
  std::unique_ptr<void, BufferDeleter> storage;
  int32_t width = 0;
  int32_t height = 0;
  int64_t timestamp_ns = 0;
};

using ImageCallback = void (BIONIC_SYSV*)(void*, AImageReader*);

struct ReaderState {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::unique_ptr<ImageFrame>> pending;
  std::unordered_set<AImage*> acquired;
  uint64_t next_frame = 1;
  size_t notifications = 0;
  ImageCallback callback = nullptr;
  void* context = nullptr;
  bool closed = false;
};

// The producer accepts RGBA CPU writes, including frames rendered through the
// existing Surface canvas and decoder paths. Raw GPU buffer dequeue requires
// a producer with graphics buffer handles and fence-aware slot reuse.
class ReaderWindow final : public QueueSurface {
 public:
  ReaderWindow(int32_t width, int32_t height) : QueueSurface(width, height) {}

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
        supported = format == 0 || format == kFormat;
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
          supported = supported && (format == 0 || format == kFormat);
        }
        break;
      }
      case NATIVE_WINDOW_GET_CONSUMER_USAGE64:
        *va_arg(probe, uint64_t*) = kUsage;
        va_end(probe);
        return 0;
    }
    va_end(probe);
    return supported ? QueueSurface::perform(operation, args) : -EINVAL;
  }
};

int Publish(const std::weak_ptr<ReaderState>& weak, const SurfaceFrame& frame) {
  auto state = weak.lock();
  if (state == nullptr) return -ENODEV;

  const bool valid_size = ValidSize(frame.width, frame.height);
  std::unique_ptr<void, BufferDeleter> storage;
  if (valid_size) {
    storage.reset(ahb::AllocateRgbaBuffer(frame.width, frame.height, false));
    if (storage) {
      std::memcpy(ahb::RgbaBufferPixels(storage.get()), frame.pixels.data(),
                  frame.pixels.size() * sizeof(uint32_t));
      ahb::MarkRgbaWritten(storage.get());
    }
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  // QueueSurface hands us each specific frame after releasing its producer
  // lock. A later producer can finish copying before an earlier one.
  state->changed.wait(lock, [&] { return state->closed || state->next_frame == frame.number; });
  if (state->closed) return -ENODEV;
  ++state->next_frame;
  state->changed.notify_all();
  if (!storage) return valid_size ? -ENOMEM : -EINVAL;
  if (state->pending.size() == kPendingFrames) state->pending.pop_front();
  state->pending.push_back(
      std::make_unique<ImageFrame>(ImageFrame{std::move(storage), frame.width, frame.height,
                                              frame.timestamp_ns}));
  if (state->callback != nullptr && state->notifications < kPendingFrames) ++state->notifications;
  return 0;
}
}  // namespace

struct AImageReader {
  const int32_t width;
  const int32_t height;
  const int32_t max_images;
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
      format != kFormat || usage != kUsage) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  auto state = std::make_shared<ReaderState>();
  auto window = android::sp<ReaderWindow>::make(width, height);
  window->SetForwardingListener([weak = std::weak_ptr<ReaderState>(state)](const SurfaceFrame& frame) {
    return Publish(weak, frame);
  });
  *out = new (std::nothrow) AImageReader{width, height, max_images, std::move(state),
                                        std::move(window), {}, false};
  return *out != nullptr ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

media_status_t BIONIC_SYSV New(int32_t width, int32_t height, int32_t format, int32_t max_images,
                             AImageReader** out) {
  return NewWithUsage(width, height, format, kUsage, max_images, out);
}

void BIONIC_SYSV DeleteReader(AImageReader* reader) {
  if (reader == nullptr) return;
  const auto state = reader->state;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->pending.clear();
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

media_status_t BIONIC_SYSV GetReaderFormat(const AImageReader* reader, int32_t* out) {
  if (reader == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = kFormat;
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
    while (state->pending.size() > 1) state->pending.pop_front();
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
    state->changed.wait(lock, [&] { return state->closed || state->notifications != 0; });
    if (state->closed) return nullptr;
    --state->notifications;
    auto callback = state->callback;
    void* context = state->context;
    lock.unlock();
    callback(context, reader);
    lock.lock();
  }
}

media_status_t BIONIC_SYSV SetImageListener(AImageReader* reader,
                                           AImageReader_ImageListener* listener) {
  if (reader == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  void* address = listener != nullptr ? reinterpret_cast<void*>(listener->onImageAvailable) : nullptr;
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
  state->callback = reinterpret_cast<ImageCallback>(address);
  state->context = listener != nullptr ? listener->context : nullptr;
  state->notifications = 0;
  return AMEDIA_OK;
}

void BIONIC_SYSV DeleteImage(AImage* image) {
  if (image == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(image->owner->mutex);
    image->owner->acquired.erase(image);
  }
  delete image;
}

template <class T, T ImageFrame::*Field>
media_status_t BIONIC_SYSV GetImageField(const AImage* image, T* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = image->frame.get()->*Field;
  return AMEDIA_OK;
}

template <int32_t Value>
media_status_t BIONIC_SYSV GetImageValue(const AImage* image, int32_t* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = Value;
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV GetCropRect(const AImage* image, AImageCropRect* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = {0, 0, image->frame->width, image->frame->height};
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV GetPixelStride(const AImage* image, int plane, int32_t* out) {
  if (plane != 0) return AMEDIA_ERROR_INVALID_PARAMETER;
  return GetImageValue<kPixelStride>(image, out);
}

media_status_t BIONIC_SYSV GetRowStride(const AImage* image, int plane, int32_t* out) {
  if (plane != 0) return AMEDIA_ERROR_INVALID_PARAMETER;
  const media_status_t result = GetImageField<int32_t, &ImageFrame::width>(image, out);
  if (result == AMEDIA_OK) *out *= kPixelStride;
  return result;
}

media_status_t BIONIC_SYSV GetPlaneData(const AImage* image, int plane, uint8_t** out, int* length) {
  if (image == nullptr || plane != 0 || out == nullptr || length == nullptr) {
    return AMEDIA_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  *out = ahb::RgbaBufferPixels(image->frame->storage.get());
  *length = image->frame->width * image->frame->height * kPixelStride;
  return AMEDIA_OK;
}

media_status_t BIONIC_SYSV GetHardwareBuffer(const AImage* image, AHardwareBuffer** out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard<std::mutex> lock(image->owner->mutex);
  if (!image->frame) return AMEDIA_ERROR_INVALID_OBJECT;
  // Borrowed for the lifetime of this image. An explicit AHardwareBuffer_acquire
  // keeps the pixel storage independently of the image and reader.
  *out = static_cast<AHardwareBuffer*>(image->frame->storage.get());
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
  ENTRY(AImageReader_getFormat, GetReaderFormat);
  ENTRY(AImageReader_acquireNextImage, AcquireNext);
  ENTRY(AImageReader_acquireLatestImage, AcquireLatest);
  ENTRY(AImageReader_acquireNextImageAsync, AcquireNextAsync);
  ENTRY(AImageReader_acquireLatestImageAsync, AcquireLatestAsync);
  ENTRY(AImageReader_setImageListener, SetImageListener);
  ENTRY(AImage_delete, DeleteImage);
  ENTRY(AImage_getWidth, GetImageField<int32_t, &ImageFrame::width>);
  ENTRY(AImage_getHeight, GetImageField<int32_t, &ImageFrame::height>);
  ENTRY(AImage_getFormat, GetImageValue<kFormat>);
  ENTRY(AImage_getTimestamp, GetImageField<int64_t, &ImageFrame::timestamp_ns>);
  ENTRY(AImage_getCropRect, GetCropRect);
  ENTRY(AImage_getNumberOfPlanes, GetImageValue<1>);
  ENTRY(AImage_getPlanePixelStride, GetPixelStride);
  ENTRY(AImage_getPlaneRowStride, GetRowStride);
  ENTRY(AImage_getPlaneData, GetPlaneData);
  ENTRY(AImage_getHardwareBuffer, GetHardwareBuffer);
#undef ENTRY
  return true;
}

[[maybe_unused]] const bool g_registered = Register();
}  // namespace
