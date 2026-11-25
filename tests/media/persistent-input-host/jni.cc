// Complete production persistent JNI and InputSurface are separate translation
// units. These are only the host Surface/native-window and consumer observers.
#include "compat/media/jni/persistent_input_surface.h"
#include "compat/media/surface/queue_surface.h"
#include <android_runtime/android_view_Surface.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>

namespace {
using namespace gesher::media;
struct Lease {
  std::unique_ptr<InputSurface> input;
  std::atomic<int> frames{0};
};
std::mutex leases_mu, gate_mu;
std::condition_variable gate_cv;
std::map<jlong, std::shared_ptr<Lease>> leases;
jlong next_lease = 1;
bool gate_armed = false, gate_entered = false, gate_open = false;
bool fail_wrapper = false;
std::atomic<int> wrappers{0};

void Throw(JNIEnv* env, const char* cls, const char* why) {
  jclass type = env->FindClass(cls);
  if (type == nullptr) return;
  env->ThrowNew(type, why); env->DeleteLocalRef(type);
}
std::shared_ptr<Lease> Find(jlong id) {
  std::lock_guard<std::mutex> lock(leases_mu);
  auto it = leases.find(id);
  return it == leases.end() ? nullptr : it->second;
}
jlong JNICALL Attach(JNIEnv* env, jclass, jobject surface, jint w, jint h) {
  auto source = GetPersistentInputSurface(env, surface);
  if (!source) return 0;
  auto lease = std::make_shared<Lease>();
  lease->input = source->Attach(w, h);
  if (!lease->input) {
    Throw(env, "java/lang/IllegalStateException", "source already consumed"); return 0;
  }
  std::weak_ptr<Lease> observed = lease;
  lease->input->SetCallback([observed](const uint32_t* pixels, int width, int height, int64_t) {
    auto owner = observed.lock();
    if (!owner || width <= 0 || height <= 0 || pixels[0] != 0xff12ab34) std::_Exit(95);
    ++owner->frames;
    std::unique_lock<std::mutex> lock(gate_mu);
    if (!gate_armed) return;
    gate_entered = true; gate_cv.notify_all();
    if (!gate_cv.wait_for(lock, std::chrono::seconds(10), [] { return gate_open; })) std::_Exit(96);
    gate_armed = false;
  });
  std::lock_guard<std::mutex> lock(leases_mu);
  const jlong id = next_lease++;
  leases.emplace(id, std::move(lease));
  return id;
}
void JNICALL Close(JNIEnv*, jclass, jlong id) {
  std::shared_ptr<Lease> retired;
  {
    std::lock_guard<std::mutex> lock(leases_mu);
    auto it = leases.find(id);
    if (it == leases.end()) return;
    retired = std::move(it->second); leases.erase(it);
  }
  retired->input->Abandon();
}
jint JNICALL Frames(JNIEnv*, jclass, jlong id) {
  auto lease = Find(id); return lease ? lease->frames.load() : -1;
}
void JNICALL NativeRelease(JNIEnv*, jclass, jlong object) {
  delete reinterpret_cast<android::sp<android::Surface>*>(object);
  --wrappers;
}
jboolean JNICALL Post(JNIEnv* env, jclass, jobject surface) {
  jclass cls = env->FindClass("android/view/Surface");
  if (!cls) return false;
  jfieldID lock_field = env->GetFieldID(cls, "mLock", "Ljava/lang/Object;");
  jfieldID native_field = env->GetFieldID(cls, "mNativeObject", "J");
  jobject lock = env->GetObjectField(surface, lock_field);
  android::sp<android::Surface> owner;
  if (env->MonitorEnter(lock) == JNI_OK) {
    auto* value = reinterpret_cast<android::sp<android::Surface>*>(env->GetLongField(surface, native_field));
    if (value) owner = *value;
    env->MonitorExit(lock);
  }
  env->DeleteLocalRef(lock); env->DeleteLocalRef(cls);
  if (owner.get() == nullptr) return false;
  auto* queue = static_cast<QueueSurface*>(owner.get());
  ANativeWindow_Buffer buffer{};
  if (queue->lock(&buffer, nullptr) != 0) return false;
  static_cast<uint32_t*>(buffer.bits)[0] = 0xff12ab34;
  return queue->unlockAndPost() == 0;
}
jint JNICALL Count(JNIEnv*, jclass, jint which) {
  return which == 0 ? QueueSurface::live.load() : which == 1 ? QueueSurface::created.load() : wrappers.load();
}
void JNICALL Gate(JNIEnv*, jclass, jint operation) {
  std::unique_lock<std::mutex> lock(gate_mu);
  if (operation == 0) { gate_armed = true; gate_entered = gate_open = false; }
  if (operation == 1 && !gate_cv.wait_for(lock, std::chrono::seconds(5), [] { return gate_entered; })) std::_Exit(97);
  if (operation == 2) { gate_open = true; gate_cv.notify_all(); }
}
void JNICALL FailWrapper(JNIEnv*, jclass, jboolean value) { fail_wrapper = value; }
@JNI_CREATE@
@JNI_RELEASE@
}  // namespace

namespace android {
jobject android_view_Surface_createFromSurface(JNIEnv* env, const sp<Surface>& surface) {
  if (fail_wrapper) { Throw(env, "java/lang/OutOfMemoryError", "declared wrapper failure"); return nullptr; }
  jclass cls = env->FindClass("android/view/Surface");
  if (!cls) return nullptr;
  jmethodID ctor = env->GetMethodID(cls, "<init>", "(J)V");
  auto* reference = new sp<Surface>(surface);
  jobject result = ctor ? env->NewObject(cls, ctor, reinterpret_cast<jlong>(reference)) : nullptr;
  if (result) ++wrappers; else delete reference;
  env->DeleteLocalRef(cls);
  return result;
}
}  // namespace android

#define NATIVE(fn) reinterpret_cast<void*>(&(fn))
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  const JNINativeMethod surface_methods[] = {{"nativeRelease", "(J)V", NATIVE(NativeRelease)}};
  jclass surface = env->FindClass("android/view/Surface");
  if (!surface || env->RegisterNatives(surface, surface_methods, 1) != JNI_OK) return JNI_ERR;
  env->DeleteLocalRef(surface);
  const JNINativeMethod codec_methods[] = {
    {"native_createPersistentInputSurface", "()Landroid/media/MediaCodec$PersistentSurface;", NATIVE(MC_createPersistentInputSurface)},
    {"native_releasePersistentInputSurface", "(Landroid/view/Surface;)V", NATIVE(MC_releasePersistentInputSurface)},
  };
  jclass codec = env->FindClass("android/media/MediaCodec");
  if (!codec || env->RegisterNatives(codec, codec_methods, 2) != JNI_OK) return JNI_ERR;
  env->DeleteLocalRef(codec);
  const JNINativeMethod methods[] = {
    {"attach", "(Landroid/view/Surface;II)J", NATIVE(Attach)}, {"closeLease", "(J)V", NATIVE(Close)},
    {"frames", "(J)I", NATIVE(Frames)}, {"post", "(Landroid/view/Surface;)Z", NATIVE(Post)},
    {"count", "(I)I", NATIVE(Count)}, {"gate", "(I)V", NATIVE(Gate)}, {"failWrapper", "(Z)V", NATIVE(FailWrapper)},
  };
  jclass host = env->FindClass("PersistentInputHost");
  if (!host || env->RegisterNatives(host, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK) return JNI_ERR;
  env->DeleteLocalRef(host);
  return JNI_VERSION_1_6;
}
