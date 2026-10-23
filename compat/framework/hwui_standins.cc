// Gesher: stand-ins for hwui's natives, until views are drawn.
//
// The tables are generated from hwui's JNI sources (gen_hwui_standins.py):
// every native answers zero, false, null or 0.0, except the few below, which
// create an object or return a function. They are System V functions, as ART
// calls natives (docs/decisions/0006-managed-abi.md); the shared answers take
// no parameters, which System V's caller-pops convention allows for any
// signature. register_gesher_hwui_standins registers each table whose class
// is on the class path.

#include <jni.h>
#include <stdlib.h>

#include <log/log.h>
#include <nativehelper/JNIHelp.h>

namespace gesher_hwui {

struct StandinTable {
  const char* className;
  const JNINativeMethod* methods;
  int count;
};
extern const StandinTable kStandinTables[];
extern const int kStandinTableCount;

__attribute__((sysv_abi)) jlong Standin_zero() {
  return 0;
}

__attribute__((sysv_abi)) jdouble Standin_zeroFloat() {
  return 0.0;
}

// Non-zero defaults (gen_hwui_standins.py's SPECIALS). A float result travels
// in the low 32 bits of xmm0, so Standin_oneFloat puts 1.0f's bits there.
__attribute__((sysv_abi)) jlong Standin_true() {
  return JNI_TRUE;
}

__attribute__((sysv_abi)) jdouble Standin_oneFloat() {
  union {
    jdouble d;
    jfloat f[2];
  } u = {};
  u.f[0] = 1.0f;
  return u.d;
}

// The objects are opaque to Java; a small zeroed block stands for each, freed
// by NativeAllocationRegistry with Standin_free (Microsoft ABI, as libcore
// calls it).
static void Standin_free(void* ptr) {
  free(ptr);
}

__attribute__((sysv_abi)) jlong Standin_getFreeFunction() {
  return reinterpret_cast<jlong>(&Standin_free);
}

// RenderNode.nCreate(String name): the node, with nothing recorded.
__attribute__((sysv_abi)) jlong RenderNode_create() {
  return reinterpret_cast<jlong>(calloc(1, 64));
}

}  // namespace gesher_hwui

int register_gesher_hwui_standins(JNIEnv* env) {
  for (int i = 0; i < gesher_hwui::kStandinTableCount; i++) {
    const auto& t = gesher_hwui::kStandinTables[i];
    jclass clazz = env->FindClass(t.className);
    if (clazz == nullptr) {
      env->ExceptionClear();
      ALOGI("libandroid_runtime: %s not on the class path; its hwui stand-ins skipped",
            t.className);
      continue;
    }
    env->DeleteLocalRef(clazz);
    if (jniRegisterNativeMethods(env, t.className, t.methods, t.count) < 0) {
      ALOGE("libandroid_runtime: registering hwui stand-ins for %s failed", t.className);
      return -1;
    }
  }
  return 0;
}
