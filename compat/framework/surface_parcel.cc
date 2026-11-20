// gesher: android.view.Surface in Parcels (Surface.nativeWriteToParcel and
// nativeReadFromParcel, which the host's android_view_Surface.cpp leaves out).
//
// On Android a parceled Surface is its IGraphicBufferProducer: the receiver
// queues buffers to the same consumer. Here a SurfaceView's or
// SurfaceTexture's Surface is a QueueSurface (compat/media/surface), whose
// frames hwui draws in its own process. Its parcel is a producer endpoint, a
// binder in the consumer's process (SurfaceSink). The receiver's Surface is a
// QueueSurface of its own whose every frame goes to that endpoint: the pixels
// in a memfd the producer reuses, the frame's size in the transaction. The
// endpoint queues the frame to the consumer's QueueSurface, as a producer in
// its process would, and answers with the consumer's current size, which
// becomes the producer's default size. Chromium's GPU process draws into the
// browser's SurfaceView this way. Other surfaces (a window's) do not travel:
// they are written as no surface, which reads as an invalid Surface.

#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <map>
#include <mutex>

#include <binder/Binder.h>
#include <binder/IBinder.h>
#include <binder/Parcel.h>
#include <log/log.h>
#include <utils/RefBase.h>

#include "android_os_Parcel.h"

// compat/media/surface/surface_jni.cc
extern "C" bool gesher_surface_queue_size(jlong native_object, int* width, int* height);
extern "C" bool gesher_surface_post(jlong native_object, int width, int height, int stride,
                                    const uint32_t* pixels);
using GesherSurfaceFrameFn = void (*)(void* context, int width, int height,
                                      const uint32_t* pixels);
extern "C" jlong gesher_surface_create_forwarding(int width, int height,
                                                  GesherSurfaceFrameFn on_frame, void* context);
extern "C" void gesher_surface_set_default_size(jlong native_object, int width, int height);
extern "C" android::RefBase* gesher_surface_refbase(jlong native_object);

namespace android {
namespace {

constexpr int32_t kParcelMagic = 0x46525347;  // "GSRF"
constexpr uint32_t kFrame = IBinder::FIRST_CALL_TRANSACTION;

// The consumer's endpoint: holds its Surface weakly, as a producer does not
// keep a consumer alive.
class SurfaceSink : public BBinder {
 public:
  explicit SurfaceSink(jlong surface)
      : surface_(surface), weak_(gesher_surface_refbase(surface)->createWeak(this)) {}

  status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
    if (code != kFrame) return BBinder::onTransact(code, data, reply, flags);
    int32_t width = 0, height = 0;
    if (data.readInt32(&width) != OK || data.readInt32(&height) != OK || width <= 0 ||
        height <= 0 || width > 16384 || height > 16384) {
      return BAD_VALUE;
    }
    const int fd = data.readFileDescriptor();
    if (fd < 0) return BAD_VALUE;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (!weak_->attemptIncStrong(this)) return DEAD_OBJECT;
    status_t status = OK;
    void* pixels = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      status = NO_MEMORY;
    } else {
      if (!gesher_surface_post(surface_, width, height, width, static_cast<const uint32_t*>(pixels))) {
        status = DEAD_OBJECT;
      }
      munmap(pixels, bytes);
    }
    int current_width = 0, current_height = 0;
    gesher_surface_queue_size(surface_, &current_width, &current_height);
    gesher_surface_refbase(surface_)->decStrong(this);
    if (reply != nullptr) {
      reply->writeInt32(current_width);
      reply->writeInt32(current_height);
    }
    return status;
  }

  bool Alive() {
    if (!weak_->attemptIncStrong(this)) return false;
    gesher_surface_refbase(surface_)->decStrong(this);
    return true;
  }

 protected:
  ~SurfaceSink() override { weak_->decWeak(this); }

 private:
  const jlong surface_;
  RefBase::weakref_type* const weak_;
};

// One endpoint per consumer Surface, so that a Surface parceled again is the
// same producer endpoint (Surface.readFromParcel then keeps its object).
std::mutex gSinksMutex;
std::map<jlong, wp<SurfaceSink>> gSinks;

sp<SurfaceSink> SinkFor(jlong surface) {
  std::lock_guard<std::mutex> lock(gSinksMutex);
  for (auto it = gSinks.begin(); it != gSinks.end();) {
    sp<SurfaceSink> existing = it->second.promote();
    if (existing == nullptr || !existing->Alive()) {
      it = gSinks.erase(it);
      continue;
    }
    if (it->first == surface) return existing;
    ++it;
  }
  sp<SurfaceSink> sink = new SurfaceSink(surface);
  gSinks[surface] = sink;
  return sink;
}

// The producer side: a frame's pixels go to the endpoint through a memfd.
struct Forwarder {
  sp<IBinder> sink;
  jlong surface = 0;
  RefBase::weakref_type* weak = nullptr;  // its Surface, never released
  std::mutex mutex;
  int fd = -1;
  size_t capacity = 0;
  void* map = nullptr;
};

std::mutex gForwardersMutex;
std::map<jlong, Forwarder*> gForwarders;  // the receiver's Surface -> its producer

// The producer of a live forwarding Surface; gForwardersMutex held. An entry
// whose Surface is gone (another may now have its address) is dropped.
Forwarder* LiveForwarder(jlong surface) {
  auto it = gForwarders.find(surface);
  if (it == gForwarders.end()) return nullptr;
  Forwarder* f = it->second;
  if (f->weak->attemptIncStrong(f)) {
    gesher_surface_refbase(surface)->decStrong(f);
    return f;
  }
  gForwarders.erase(it);
  return nullptr;
}

void Forward(void* context, int width, int height, const uint32_t* pixels) {
  auto* f = static_cast<Forwarder*>(context);
  std::lock_guard<std::mutex> lock(f->mutex);
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  if (bytes > f->capacity) {
    if (f->map != nullptr) munmap(f->map, f->capacity);
    if (f->fd >= 0) close(f->fd);
    f->map = nullptr;
    f->capacity = 0;
    f->fd = memfd_create("gesher-surface", MFD_CLOEXEC);
    if (f->fd < 0 || ftruncate(f->fd, static_cast<off_t>(bytes)) != 0) {
      ALOGW("gesher-surface: no memory for a %dx%d frame", width, height);
      return;
    }
    f->map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->map == MAP_FAILED) {
      f->map = nullptr;
      return;
    }
    f->capacity = bytes;
  }
  memcpy(f->map, pixels, bytes);
  Parcel data, reply;
  data.writeInt32(width);
  data.writeInt32(height);
  data.writeFileDescriptor(f->fd);
  if (f->sink->transact(kFrame, data, &reply) != OK) return;
  int32_t current_width = 0, current_height = 0;
  if (reply.readInt32(&current_width) == OK && reply.readInt32(&current_height) == OK &&
      current_width > 0 && current_height > 0 && (current_width != width || current_height != height)) {
    gesher_surface_set_default_size(f->surface, current_width, current_height);
  }
}

void JNICALL Surface_writeToParcel(JNIEnv* env, jclass, jlong native_object, jobject parcel_object) {
  Parcel* parcel = parcelForJavaObject(env, parcel_object);
  if (parcel == nullptr) return;
  parcel->writeInt32(kParcelMagic);
  int width = 0, height = 0;
  if (native_object == 0 || !gesher_surface_queue_size(native_object, &width, &height)) {
    parcel->writeInt32(0);
    return;
  }
  // A forwarding Surface parceled again stands for the same consumer.
  std::lock_guard<std::mutex> lock(gForwardersMutex);
  Forwarder* forwarder = LiveForwarder(native_object);
  parcel->writeInt32(1);
  parcel->writeInt32(width);
  parcel->writeInt32(height);
  parcel->writeStrongBinder(forwarder != nullptr ? forwarder->sink
                                                 : sp<IBinder>(SinkFor(native_object)));
}

jlong JNICALL Surface_readFromParcel(JNIEnv* env, jclass, jlong native_object, jobject parcel_object) {
  Parcel* parcel = parcelForJavaObject(env, parcel_object);
  if (parcel == nullptr) return 0;
  int32_t magic = 0, present = 0, width = 0, height = 0;
  if (parcel->readInt32(&magic) != OK || magic != kParcelMagic || parcel->readInt32(&present) != OK ||
      present == 0 || parcel->readInt32(&width) != OK || parcel->readInt32(&height) != OK) {
    return 0;
  }
  sp<IBinder> sink = parcel->readStrongBinder();
  if (sink == nullptr) return 0;
  std::lock_guard<std::mutex> lock(gForwardersMutex);
  if (native_object != 0) {
    Forwarder* current = LiveForwarder(native_object);
    if (current != nullptr && current->sink == sink) return native_object;
  }
  // The consumer is in this process: its own Surface, not a copy of its frames.
  if (sp<BBinder> local = sink->localBinder(); local != nullptr) {
    std::lock_guard<std::mutex> sinks_lock(gSinksMutex);
    for (const auto& [surface, weak] : gSinks) {
      sp<SurfaceSink> candidate = weak.promote();
      if (candidate != nullptr && candidate.get() == local.get() && candidate->Alive()) {
        gesher_surface_refbase(surface)->incStrong(reinterpret_cast<void*>(&Surface_readFromParcel));
        return surface;
      }
    }
  }
  // Lives as long as the process: the producer may outlive this Java Surface.
  auto* forwarder = new Forwarder;
  forwarder->sink = sink;
  const jlong surface = gesher_surface_create_forwarding(width > 0 ? width : 1,
                                                         height > 0 ? height : 1, Forward, forwarder);
  forwarder->surface = surface;
  forwarder->weak = gesher_surface_refbase(surface)->createWeak(forwarder);
  gForwarders[surface] = forwarder;
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
