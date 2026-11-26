// gesher: guest JNI access to Java HardwareBuffers.
//
// AHardwareBuffer_fromHardwareBuffer, as Flutter's raster thread imports a
// platform view's frame (Image.getHardwareBuffer(), then the NDK buffer,
// eglGetNativeClientBufferANDROID / EGL_NATIVE_BUFFER_ANDROID in
// egl_native_buffer.cc, and a texture). A Java HardwareBuffer, created by the
// app or obtained from ImageReader, owns a reference to its backing buffer.
// As the NDK specifies, fromHardwareBuffer returns it without acquiring another.
//
// android.hardware.SyncFence's natives, over Gesher's native fences
// (gesher/native_fence.h): Image.getFence() and an app's SyncFence of a fence
// descriptor (eglDupNativeFenceFDANDROID's).
#include "abi_types.h"
#include "jni_bridge.h"

#include "../media/surface/reader_surface.h"

#include <gesher/bionic_shim.h>
#include <gesher/native_fence.h>

#include <nativehelper/JNIHelp.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace gesher::bionic {
namespace {

// The JNIEnv is the loaded library's System V proxy; the Java object is read
// through the runtime's own.
void* BIONIC_SYSV FromHardwareBuffer(JNIEnv* env, jobject hardware_buffer) {
  if (env == nullptr || hardware_buffer == nullptr) return nullptr;
  return media::HardwareBufferNative(RealJniEnv(env), hardware_buffer);
}

[[maybe_unused]] const bool g_registered = [] {
  AddBionicSymbol("AHardwareBuffer_fromHardwareBuffer", reinterpret_cast<void*>(&FromHardwareBuffer));
  return true;
}();

// --- SyncFence -------------------------------------------------------------------------------

// What SyncFence's native pointer is: a reference-counted native fence and the
// descriptor it was made from (owned: SyncFence.create detached it for us).
struct Fence {
  std::atomic<int> refs{1};
  int fd;
  std::shared_ptr<NativeFence> native;
};

Fence* FenceOf(jlong ptr) { return reinterpret_cast<Fence*>(ptr); }

void FenceRelease(void* ptr) {
  auto* fence = static_cast<Fence*>(ptr);
  if (fence->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    close(fence->fd);
    delete fence;
  }
}

jlong JNICALL FenceDestructor(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&FenceRelease); }

jlong JNICALL FenceCreate(JNIEnv* env, jclass, jint fd) {
  std::shared_ptr<NativeFence> native = LookupNativeFence(fd);
  if (native == nullptr) {
    close(fd);  // ours: SyncFence detached it
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "the descriptor is not a native fence of this device");
    return 0;
  }
  auto* fence = new Fence{{1}, fd, std::move(native)};
  return reinterpret_cast<jlong>(fence);
}

jboolean JNICALL FenceValid(JNIEnv*, jclass, jlong ptr) { return FenceOf(ptr)->fd >= 0; }
jint JNICALL FenceFd(JNIEnv*, jclass, jlong ptr) { return FenceOf(ptr)->fd; }

jboolean JNICALL FenceWait(JNIEnv*, jclass, jlong ptr, jlong timeout_ns) {
  // A negative timeout waits forever.
  const uint64_t timeout = timeout_ns < 0 ? UINT64_MAX : static_cast<uint64_t>(timeout_ns);
  return WaitNativeFence(FenceOf(ptr)->native, timeout) == FenceStatus::kSignaled ? JNI_TRUE : JNI_FALSE;
}

// A pending fence is SIGNAL_TIME_PENDING; Gesher's fences do not record when they
// signaled, so a signaled one has no time to give (SIGNAL_TIME_INVALID, as an error).
jlong JNICALL FenceSignalTime(JNIEnv*, jclass, jlong ptr) {
  return NativeFenceStatus(FenceOf(ptr)->native) == FenceStatus::kPending ? INT64_MAX : -1;
}

void JNICALL FenceIncRef(JNIEnv*, jclass, jlong ptr) {
  FenceOf(ptr)->refs.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

// From register_gesher_reader_buffers (reader_surface.cc): SyncFence's natives.
int RegisterSyncFenceNatives(JNIEnv* env) {
#define N(fn) reinterpret_cast<void*>(&(fn))
  static const JNINativeMethod methods[] = {
      {"nGetDestructor", "()J", N(FenceDestructor)}, {"nCreate", "(I)J", N(FenceCreate)},
      {"nIsValid", "(J)Z", N(FenceValid)},           {"nGetFd", "(J)I", N(FenceFd)},
      {"nWait", "(JJ)Z", N(FenceWait)},              {"nGetSignalTime", "(J)J", N(FenceSignalTime)},
      {"nIncRef", "(J)V", N(FenceIncRef)},
  };
#undef N
  jclass cls = env->FindClass("android/hardware/SyncFence");
  if (cls == nullptr) {
    env->ExceptionClear();  // not on this class path
    return 0;
  }
  int result = env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0]));
  env->DeleteLocalRef(cls);
  return result;
}

}  // namespace gesher::bionic
