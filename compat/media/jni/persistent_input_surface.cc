// MediaCodec.PersistentSurface's producer survives successive codec/recorder
// consumers. Its Java long is an opaque, never reused registry token, not a raw
// pointer that a concurrent release could leave dangling.
#include "persistent_input_surface.h"

#include <limits>
#include <map>
#include <mutex>

namespace gesher::media {
namespace {

std::mutex registry_mu;
std::map<jlong, std::shared_ptr<PersistentInputSurface>> registry;
jlong next_token = 1;

void Throw(JNIEnv* env, const char* name, const char* message) {
  if (env->ExceptionCheck()) return;
  jclass cls = env->FindClass(name);
  if (cls == nullptr) return;
  env->ThrowNew(cls, message);
  env->DeleteLocalRef(cls);
}

struct Fields {
  explicit Fields(JNIEnv* e) : env(e) {
    cls = env->FindClass("android/media/MediaCodec$PersistentSurface");
    if (cls == nullptr) return;
    token = env->GetFieldID(cls, "mPersistentObject", "J");
    if (token != nullptr) lock = env->GetFieldID(cls, "mLock", "Ljava/lang/Object;");
  }
  ~Fields() { if (cls != nullptr) env->DeleteLocalRef(cls); }
  bool valid() const { return token != nullptr && lock != nullptr; }
  bool Accept(jobject surface) const {
    if (!valid()) return false;
    if (surface != nullptr && env->IsInstanceOf(surface, cls)) return true;
    Throw(env, "java/lang/IllegalArgumentException", "not a PersistentSurface");
    return false;
  }
  JNIEnv* env;
  jclass cls = nullptr;
  jfieldID token = nullptr, lock = nullptr;
};

struct SurfaceLock {
  SurfaceLock(JNIEnv* e, jobject surface, const Fields& fields) : env(e) {
    object = env->GetObjectField(surface, fields.lock);
    if (object != nullptr) held = env->MonitorEnter(object) == JNI_OK;
  }
  ~SurfaceLock() {
    if (held) env->MonitorExit(object);
    if (object != nullptr) env->DeleteLocalRef(object);
  }
  JNIEnv* env;
  jobject object = nullptr;
  bool held = false;
};

// Factory failure must release the real Surface reference even when Java has
// already raised an exception. Invoke Surface's own release, not the override
// whose persistent token may not have been installed yet.
void CloseSurface(JNIEnv* env, jobject surface, jclass cls, jmethodID release) {
  if (surface == nullptr) return;
  jthrowable pending = env->ExceptionOccurred();
  if (pending != nullptr) env->ExceptionClear();
  env->CallNonvirtualVoidMethod(surface, cls, release);
  if (pending != nullptr) {
    env->ExceptionClear();
    env->Throw(pending);
    env->DeleteLocalRef(pending);
  }
}

}  // namespace

jobject NewPersistentInputSurface(JNIEnv* env) {
  Fields fields(env);
  if (!fields.valid()) return nullptr;
  jmethodID constructor = env->GetMethodID(fields.cls, "<init>", "()V");
  if (constructor == nullptr) return nullptr;
  jmethodID transfer = env->GetMethodID(fields.cls, "transferFrom", "(Landroid/view/Surface;)V");
  if (transfer == nullptr) return nullptr;
  jclass surface_class = env->FindClass("android/view/Surface");
  if (surface_class == nullptr) return nullptr;
  jmethodID release = env->GetMethodID(surface_class, "release", "()V");
  if (release == nullptr) {
    env->DeleteLocalRef(surface_class);
    return nullptr;
  }
  auto source = CreatePersistentInputSurface();
  jobject result = env->NewObject(fields.cls, constructor);
  jobject wrapper = result != nullptr ? source->NewJavaSurface(env) : nullptr;
  if (wrapper != nullptr && !env->ExceptionCheck()) {
    // Surface.transferFrom moves the native producer reference and closes the
    // temporary wrapper's CloseGuard. It does not add a second native owner.
    env->CallVoidMethod(result, transfer, wrapper);
  }
  bool published = false;
  if (wrapper != nullptr && !env->ExceptionCheck()) {
    SurfaceLock lock(env, result, fields);
    if (lock.held) {
      std::lock_guard<std::mutex> registry_lock(registry_mu);
      if (next_token != 0) {
        const jlong token = next_token;
        next_token = token == std::numeric_limits<jlong>::max() ? 0 : token + 1;
        registry.emplace(token, source);
        env->SetLongField(result, fields.token, token);
        published = true;
      }
    }
  }
  if (!published) {
    Throw(env, "java/lang/OutOfMemoryError", "cannot create persistent input Surface");
    CloseSurface(env, result, surface_class, release);
    CloseSurface(env, wrapper, surface_class, release);
    if (result != nullptr) env->DeleteLocalRef(result);
    result = nullptr;
  }
  if (wrapper != nullptr) env->DeleteLocalRef(wrapper);
  env->DeleteLocalRef(surface_class);
  return result;
}

std::shared_ptr<PersistentInputSurface> GetPersistentInputSurface(JNIEnv* env, jobject surface) {
  Fields fields(env);
  if (!fields.Accept(surface)) return nullptr;
  std::shared_ptr<PersistentInputSurface> source;
  {
    SurfaceLock lock(env, surface, fields);
    if (!lock.held) return nullptr;
    const jlong token = env->GetLongField(surface, fields.token);
    std::lock_guard<std::mutex> registry_lock(registry_mu);
    auto found = registry.find(token);
    if (found != registry.end()) source = found->second;
  }
  if (source == nullptr)
    Throw(env, "java/lang/IllegalArgumentException", "persistent input Surface was released");
  return source;
}

void ReleasePersistentInputSurface(JNIEnv* env, jobject surface) {
  Fields fields(env);
  if (!fields.Accept(surface)) return;
  std::shared_ptr<PersistentInputSurface> retired;
  {
    SurfaceLock lock(env, surface, fields);
    if (!lock.held) return;
    const jlong token = env->GetLongField(surface, fields.token);
    env->SetLongField(surface, fields.token, 0);
    std::lock_guard<std::mutex> registry_lock(registry_mu);
    auto found = registry.find(token);
    if (found != registry.end()) {
      retired = std::move(found->second);
      registry.erase(found);
    }
  }
  // Destruction may quiesce a producer callback. Neither the Java monitor nor
  // the registry mutex may be held while waiting for consumer code.
}

}  // namespace gesher::media
