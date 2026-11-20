// gesher: android.view.Surface in Parcels (Surface.nativeWriteToParcel and
// nativeReadFromParcel, which the host's android_view_Surface.cpp leaves out).
//
// On Android a parceled Surface is its IGraphicBufferProducer: the receiver
// queues buffers to the same BufferQueue. Here a SurfaceView's,
// SurfaceTexture's or ImageReader's Surface is a QueueSurface
// (compat/media/surface), whose consumer is in its own process. Its parcel is a
// producer endpoint, a binder in the consumer's process (SurfaceSink), and the
// consumer's size:
//
//   int32 'GSRF', int32 present (0: no surface), int32 width, int32 height,
//   strong binder (the endpoint)
//
// The receiver's Surface is a QueueSurface of its own (a forwarding one) whose
// every frame goes to the endpoint: the pixels in a memfd it reuses, their size
// and time stamp in a synchronous transaction, which the endpoint answers once
// it has queued the frame to the consumer's QueueSurface. The answer carries
// the consumer's current size, which becomes the producer's default size (as
// libgui's Surface takes it from each queueBuffer's output), and whether the
// consumer is still there: once it is not (released, abandoned, its process
// gone), the forwarding Surface is abandoned, so that the producer's next lock
// or dequeue fails as on an abandoned BufferQueue. Read in the consumer's own
// process, the parcel is the consumer's Surface itself. A forwarding Surface
// parceled again stands for the same endpoint. Chromium's GPU process draws
// into the browser's SurfaceView this way. Other surfaces (a window's) do not
// travel: they are written as no surface, which reads as an invalid Surface.
//
// Unlike a device: a producer learns of a new consumer size, and of a consumer
// that is gone, with the frame it queues after the change (a device's next
// dequeue sees both); and Surfaces read from parcels of one consumer may all
// produce at once (a BufferQueue takes one connected producer at a time).

#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <binder/Binder.h>
#include <binder/IBinder.h>
#include <binder/Parcel.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <utils/RefBase.h>
#include <utils/String16.h>

#include "android_os_Parcel.h"
#include "../media/surface/surface_parcel_bridge.h"

namespace android {
namespace {

constexpr int32_t kParcelMagic = 0x46525347;  // "GSRF"
constexpr uint32_t kFrame = IBinder::FIRST_CALL_TRANSACTION;
// The largest frame side either end takes (a 16384x16384 frame is 1 GiB).
constexpr int32_t kMaxSide = 16384;

// The id of the reference a Java Surface holds on its native object
// (android_view_Surface.cpp's sRefBaseOwner).
const char kJavaReference = 0;

const String16& Descriptor() {
  static const String16 descriptor("com.gesher.view.ISurfaceSink");
  return descriptor;
}

// The consumer's endpoint, a binder in its process. It holds the consumer's
// Surface weakly: a producer does not keep its consumer alive.
class SurfaceSink : public BBinder {
 public:
  explicit SurfaceSink(jlong surface)
      : surface_(surface), weak_(gesher_surface_refbase(surface)->createWeak(this)) {}

  const String16& getInterfaceDescriptor() const override { return Descriptor(); }

  // Whether this is the endpoint of `surface`, a live Surface. The weak
  // references this keeps are not reused while it does, so a Surface that has
  // since taken the address of this one's has others.
  bool Serves(jlong surface) const {
    return surface == surface_ && gesher_surface_refbase(surface)->getWeakRefs() == weak_;
  }

  // The consumer's Surface with a strong reference for `id`, or 0 if it is gone.
  jlong Acquire(const void* id) const { return weak_->attemptIncStrong(id) ? surface_ : 0; }

 protected:
  ~SurfaceSink() override { weak_->decWeak(this); }

  // kFrame: int32 width, int32 height, int64 time stamp, the memfd with the
  // pixels (RGBA_8888, width pixels a row). Answered with int32 status (OK;
  // NO_INIT: the consumer is gone; BAD_VALUE: not a frame) and the consumer's
  // current size.
  status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
    if (code != kFrame) return BBinder::onTransact(code, data, reply, flags);
    if (!data.enforceInterface(Descriptor())) return PERMISSION_DENIED;
    if (reply == nullptr) return BAD_VALUE;  // the producer waits for the answer
    status_t status = NO_INIT;
    int width = 0, height = 0;
    if (const jlong surface = Acquire(this); surface != 0) {
      status = Queue(surface, data);
      gesher_surface_queue_size(surface, &width, &height);
      gesher_surface_refbase(surface)->decStrong(this);
    }
    reply->writeInt32(status);
    reply->writeInt32(width);
    reply->writeInt32(height);
    return OK;
  }

 private:
  static status_t Queue(jlong surface, const Parcel& data) {
    int32_t width = 0, height = 0;
    int64_t timestamp = 0;
    if (data.readInt32(&width) != OK || data.readInt32(&height) != OK ||
        data.readInt64(&timestamp) != OK || width <= 0 || height <= 0 || width > kMaxSide ||
        height > kMaxSide) {
      return BAD_VALUE;
    }
    // The Parcel's descriptor (it closes it).
    const int fd = data.readFileDescriptor();
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    // A memfd shorter than the frame would fault where the mapping passes its end.
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) < bytes) {
      return BAD_VALUE;
    }
    void* pixels = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) return NO_MEMORY;
    const bool queued = gesher_surface_post(surface, width, height, width, timestamp,
                                            static_cast<const uint32_t*>(pixels));
    munmap(pixels, bytes);
    return queued ? OK : NO_INIT;
  }

  const jlong surface_;
  RefBase::weakref_type* const weak_;
};

std::mutex gSinksMutex;
std::map<jlong, wp<SurfaceSink>> gSinks;  // a consumer's Surface -> its endpoint

// The endpoint of `surface` (live: a Java Surface holds it). One per Surface, so
// that a Surface parceled again is the same producer endpoint (and
// Surface.readFromParcel keeps the Surface it has).
sp<SurfaceSink> SinkFor(jlong surface) {
  std::lock_guard<std::mutex> lock(gSinksMutex);
  sp<SurfaceSink> sink;
  for (auto it = gSinks.begin(); it != gSinks.end();) {
    sp<SurfaceSink> existing = it->second.promote();
    if (existing == nullptr) {
      it = gSinks.erase(it);
      continue;
    }
    if (it->first == surface && existing->Serves(surface)) sink = existing;
    ++it;
  }
  if (sink == nullptr) {
    sink = sp<SurfaceSink>::make(surface);
    gSinks[surface] = sink;  // in place of a gone Surface's that had this address
  }
  return sink;
}

// The producer side of a Surface read from a Parcel: its frames go to the
// consumer's endpoint through a memfd it reuses. Its forwarding Surface's
// listener owns it (gesher_surface_create_forwarding): it goes with that
// Surface, or once the Surface is abandoned.
struct Forwarder {
  explicit Forwarder(sp<IBinder> endpoint) : sink(std::move(endpoint)) {}
  Forwarder(const Forwarder&) = delete;
  Forwarder& operator=(const Forwarder&) = delete;
  ~Forwarder() {
    if (map != nullptr) munmap(map, capacity);
    if (fd >= 0) close(fd);
  }

  const sp<IBinder> sink;
  // One frame at a time: the memfd is the consumer's to read until it answers.
  std::mutex mutex;
  int fd = -1;
  size_t capacity = 0;
  void* map = nullptr;
  bool warned = false;
};

std::mutex gForwardersMutex;
// A Surface read from a Parcel -> its producer side, while it has one.
std::map<jlong, std::weak_ptr<Forwarder>> gForwarders;

// The endpoint `surface` (live: a Java Surface holds it) forwards its frames
// to, or null if it is not a Surface read from a Parcel. A Forwarder goes with
// its Surface, so one found for this address is this Surface's.
sp<IBinder> ForwardedTo(jlong surface) {
  std::lock_guard<std::mutex> lock(gForwardersMutex);
  auto it = gForwarders.find(surface);
  if (it == gForwarders.end()) return nullptr;
  std::shared_ptr<Forwarder> forwarder = it->second.lock();
  if (forwarder == nullptr) {
    gForwarders.erase(it);
    return nullptr;
  }
  return forwarder->sink;
}

// A memfd of at least `bytes`, mapped; false (and none) if there cannot be one.
bool Reserve(Forwarder* f, size_t bytes) {
  if (bytes <= f->capacity) return true;
  if (f->map != nullptr) munmap(f->map, f->capacity);
  if (f->fd >= 0) close(f->fd);
  f->map = nullptr;
  f->capacity = 0;
  f->fd = memfd_create("gesher-surface", MFD_CLOEXEC);
  if (f->fd >= 0 && ftruncate(f->fd, static_cast<off_t>(bytes)) == 0) {
    void* map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (map != MAP_FAILED) {
      f->map = map;
      f->capacity = bytes;
      return true;
    }
  }
  if (f->fd >= 0) close(f->fd);
  f->fd = -1;
  return false;
}

// A frame queued to a forwarding Surface, on its producer's thread.
void Forward(void* context, jlong surface, int width, int height, int64_t timestamp,
             const uint32_t* pixels) {
  Forwarder* f = static_cast<std::shared_ptr<Forwarder>*>(context)->get();
  status_t status = NO_INIT;
  int32_t consumer_width = 0, consumer_height = 0;
  {
    std::lock_guard<std::mutex> lock(f->mutex);
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide || !Reserve(f, bytes)) {
      if (!f->warned) ALOGW("gesher-surface: no memory for a %dx%d frame", width, height);
      f->warned = true;
      return;
    }
    memcpy(f->map, pixels, bytes);
    Parcel data, reply;
    data.writeInterfaceToken(Descriptor());
    data.writeInt32(width);
    data.writeInt32(height);
    data.writeInt64(timestamp);
    data.writeFileDescriptor(f->fd);
    const status_t transport = f->sink->transact(kFrame, data, &reply);
    if (transport == OK) {
      int32_t answer = UNKNOWN_ERROR;
      if (reply.readInt32(&answer) != OK || reply.readInt32(&consumer_width) != OK ||
          reply.readInt32(&consumer_height) != OK) {
        answer = UNKNOWN_ERROR;
      }
      status = answer;
    } else if (transport != DEAD_OBJECT) {  // DEAD_OBJECT: the consumer's process is gone
      status = transport;
    }
    if (status != OK && status != NO_INIT && !f->warned) {
      ALOGW("gesher-surface: a %dx%d frame was not queued (%d)", width, height, status);
      f->warned = true;
    }
  }
  if (status == NO_INIT) {
    gesher_surface_abandon(surface);
  } else if (status == OK && consumer_width > 0 && consumer_height > 0 &&
             consumer_width <= kMaxSide && consumer_height <= kMaxSide) {
    gesher_surface_set_default_size(surface, consumer_width, consumer_height);
  }
}

void ReleaseForwarder(void* context) { delete static_cast<std::shared_ptr<Forwarder>*>(context); }

// A forwarding Surface for `sink`, with a reference for the Java Surface.
jlong CreateForwarding(const sp<IBinder>& sink, int width, int height) {
  auto forwarder = std::make_shared<Forwarder>(sink);
  const jlong surface = gesher_surface_create_forwarding(
      width > 0 ? width : 1, height > 0 ? height : 1, Forward, ReleaseForwarder,
      new std::shared_ptr<Forwarder>(forwarder));
  std::lock_guard<std::mutex> lock(gForwardersMutex);
  // Entries of Surfaces that are gone (another may have taken their address).
  std::erase_if(gForwarders, [](const auto& entry) { return entry.second.expired(); });
  gForwarders[surface] = forwarder;
  return surface;
}

// The Surface `parcel` holds, with a reference for the Java Surface reading it,
// or 0 for none; `current` (the Java Surface's own, or 0) if it is the same.
jlong ReadSurface(const Parcel& parcel, jlong current) {
  int32_t magic = 0, present = 0, width = 0, height = 0;
  if (parcel.readInt32(&magic) != OK || magic != kParcelMagic ||
      parcel.readInt32(&present) != OK || present == 0 || parcel.readInt32(&width) != OK ||
      parcel.readInt32(&height) != OK || width < 0 || height < 0 || width > kMaxSide ||
      height > kMaxSide) {
    return 0;
  }
  sp<IBinder> sink = parcel.readStrongBinder();
  if (sink == nullptr) return 0;
  // The endpoint the current Surface forwards to: that Surface (as on a device,
  // where the same IGraphicBufferProducer keeps its Surface).
  if (current != 0 && ForwardedTo(current) == sink) return current;
  // The consumer is in this process: its own Surface, not a copy of its frames.
  if (BBinder* local = sink->localBinder();
      local != nullptr && local->getInterfaceDescriptor() == Descriptor()) {
    auto* own = static_cast<SurfaceSink*>(local);
    if (current != 0 && own->Serves(current)) return current;
    if (const jlong surface = own->Acquire(&kJavaReference); surface != 0) return surface;
    // Its Surface is gone: frames from this one find no consumer, as on a device.
  }
  return CreateForwarding(sink, width, height);
}

void JNICALL Surface_writeToParcel(JNIEnv* env, jclass, jlong native_object, jobject parcel_object) {
  Parcel* parcel = parcelForJavaObject(env, parcel_object);
  if (parcel == nullptr) {
    jniThrowNullPointerException(env, nullptr);
    return;
  }
  parcel->writeInt32(kParcelMagic);
  int width = 0, height = 0;
  if (native_object == 0 || !gesher_surface_queue_size(native_object, &width, &height)) {
    parcel->writeInt32(0);
    return;
  }
  // A Surface read from a Parcel stands for the same consumer.
  sp<IBinder> sink = ForwardedTo(native_object);
  if (sink == nullptr) sink = SinkFor(native_object);
  parcel->writeInt32(1);
  parcel->writeInt32(width);
  parcel->writeInt32(height);
  parcel->writeStrongBinder(sink);
}

jlong JNICALL Surface_readFromParcel(JNIEnv* env, jclass, jlong native_object, jobject parcel_object) {
  Parcel* parcel = parcelForJavaObject(env, parcel_object);
  if (parcel == nullptr) {
    jniThrowNullPointerException(env, nullptr);
    return 0;
  }
  const jlong surface = ReadSurface(*parcel, native_object);
  // Surface.readFromParcel takes the Surface returned in place of its own,
  // whose reference goes here (as in android_view_Surface.cpp).
  if (native_object != 0 && surface != native_object) {
    gesher_surface_refbase(native_object)->decStrong(&kJavaReference);
  }
  return surface;
}

const JNINativeMethod kMethods[] = {
    {"nativeWriteToParcel", "(JLandroid/os/Parcel;)V", reinterpret_cast<void*>(&Surface_writeToParcel)},
    {"nativeReadFromParcel", "(JLandroid/os/Parcel;)J", reinterpret_cast<void*>(&Surface_readFromParcel)},
};

}  // namespace

int register_gesher_surface_parcel(JNIEnv* env) {
  jclass surface = env->FindClass("android/view/Surface");
  if (surface == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  for (const JNINativeMethod& m : kMethods) {
    if (env->RegisterNatives(surface, &m, 1) != 0) env->ExceptionClear();
  }
  env->DeleteLocalRef(surface);
  return 0;
}

}  // namespace android
