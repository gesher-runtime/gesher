// Gesher: stand-ins for device-only natives (SurfaceFlinger's SurfaceControl
// and SurfaceSession, BufferQueue's BLASTBufferQueue).
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

}  // namespace gesher_device

void gesher_set_blast_surface_provider(GesherBlastSurfaceProvider provider) {
  gesher_device::gBlastSurfaceProvider.store(provider);
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
  return reinterpret_cast<jlong>(calloc(1, 64));
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
