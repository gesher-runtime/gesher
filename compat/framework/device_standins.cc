// Gesher: stand-ins for device-only natives (SurfaceFlinger's SurfaceControl
// and SurfaceSession, BufferQueue's BLASTBufferQueue, InputDispatcher's
// InputChannel and InputEventReceiver, libgui's OverlayProperties).
//
// The tables are generated from the framework's JNI sources
// (gen_device_standins.py): every native answers zero, false, null or 0.0,
// except the ones below. They are System V functions, as ART calls natives
// (docs/decisions/0006-managed-abi.md), with C linkage so the generated tables
// can name them without their types; the shared answers take no parameters,
// which System V's caller-pops convention allows for any signature.
// register_gesher_device_standins registers each table whose class is on the
// class path. hwui's own natives are real (compat/hwui/graphics_runtime.cc).

#include <jni.h>
#include <stdlib.h>

#include <atomic>

#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include "gesher_device_standins.h"

#define SYSV __attribute__((sysv_abi))

namespace gesher_device {

struct StandinTable {
  const char* className;
  const JNINativeMethod* methods;
  int count;
};
extern const StandinTable kStandinTables[];
extern const int kStandinTableCount;

// The objects are opaque to Java; a small zeroed block stands for each, freed
// by NativeAllocationRegistry with this function (Microsoft ABI, as libcore
// calls it).
static void FreeBlock(void* ptr) {
  free(ptr);
}

static std::atomic<GesherBlastSurfaceProvider> gBlastSurfaceProvider{nullptr};

// A handle's block: its first word is its tag (gesher_device_standins.h).
constexpr size_t kBlockSize = 64;

static int64_t* Block(jlong handle) {
  return reinterpret_cast<int64_t*>(static_cast<intptr_t>(handle));
}

}  // namespace gesher_device

void gesher_set_blast_surface_provider(GesherBlastSurfaceProvider provider) {
  gesher_device::gBlastSurfaceProvider.store(provider);
}

int64_t gesher_standin_tag(int64_t handle) {
  return handle != 0 ? gesher_device::Block(handle)[0] : 0;
}

void gesher_standin_set_tag(int64_t handle, int64_t tag) {
  if (handle != 0) gesher_device::Block(handle)[0] = tag;
}

extern "C" {

SYSV jlong gesher_device_Standin_zero() {
  return 0;
}

SYSV jdouble gesher_device_Standin_zeroFloat() {
  return 0.0;
}

// SurfaceControl.nativeCreate, Transaction's, BLASTBufferQueue's: a handle with
// no state behind it (there is no compositor).
SYSV jlong gesher_device_Standin_create() {
  return reinterpret_cast<jlong>(calloc(1, gesher_device::kBlockSize));
}

// SurfaceControl.nativeGetHandle(long): the tag stands for the layer's handle,
// so copies of one SurfaceControl are the same surface (isSameSurface).
SYSV jlong gesher_device_Standin_tag(JNIEnv*, jclass, jlong handle) {
  return gesher_standin_tag(handle);
}

// SurfaceControl.nativeCopyFromSurfaceControl(long): a new handle with the
// same tag (relayout's outSurfaceControl.copyFrom).
SYSV jlong gesher_device_SurfaceControl_copy(JNIEnv*, jclass, jlong from) {
  jlong copy = gesher_device_Standin_create();
  gesher_standin_set_tag(copy, gesher_standin_tag(from));
  return copy;
}

// BLASTBufferQueue.nativeUpdate(long ptr, long surfaceControl, long width,
// long height, int format): the adapter now draws into that SurfaceControl.
SYSV void gesher_device_Blast_update(JNIEnv*, jobject, jlong ptr, jlong surface_control, jlong,
                                     jlong, jint) {
  gesher_standin_set_tag(ptr, gesher_standin_tag(surface_control));
}

// BLASTBufferQueue.nativeIsSameSurfaceControl(long ptr, long surfaceControl).
SYSV jboolean gesher_device_Blast_isSameSurfaceControl(JNIEnv*, jobject, jlong ptr,
                                                      jlong surface_control) {
  int64_t tag = gesher_standin_tag(ptr);
  return tag != 0 && tag == gesher_standin_tag(surface_control) ? JNI_TRUE : JNI_FALSE;
}

SYSV jlong gesher_device_Standin_getFreeFunction() {
  return reinterpret_cast<jlong>(&gesher_device::FreeBlock);
}

// BLASTBufferQueue.nativeGetSurface(long ptr, boolean includeSurfaceControlHandle):
// the Surface a window draws into, from the window code's provider.
SYSV jobject gesher_device_Blast_getSurface(JNIEnv* env, jobject, jlong ptr, jboolean) {
  GesherBlastSurfaceProvider provider = gesher_device::gBlastSurfaceProvider.load();
  return provider != nullptr ? provider(env, ptr) : nullptr;
}

}  // extern "C"

int register_gesher_device_standins(JNIEnv* env) {
  for (int i = 0; i < gesher_device::kStandinTableCount; i++) {
    const auto& t = gesher_device::kStandinTables[i];
    jclass clazz = env->FindClass(t.className);
    if (clazz == nullptr) {
      env->ExceptionClear();
      ALOGI("libandroid_runtime: %s not on the class path; its stand-ins skipped", t.className);
      continue;
    }
    env->DeleteLocalRef(clazz);
    if (jniRegisterNativeMethods(env, t.className, t.methods, t.count) < 0) {
      ALOGE("libandroid_runtime: registering the stand-ins for %s failed", t.className);
      return -1;
    }
  }
  return 0;
}
