// Gesher: the host libandroid_runtime, linked into the executable.
//
// frameworks/base's AndroidRuntime.cpp is not reused (its registration table
// references every host source and hwui). This file holds the AndroidRuntime
// statics the framework's natives use and a fixed registration table for the
// headless framework (docs/framework-natives.md), and registers itself as the
// built-in library "libandroid_runtime.so" with gesher-base's dlopen, the way
// compat/libcore/builtins.cc registers libcore's libraries. Java code reaches
// it through System.loadLibrary("android_runtime"), whose JNI_OnLoad (here
// JNI_OnLoad_android_runtime, the convention for statically linked libraries)
// runs the registrations.
//
// ART enters natives with the System V ABI (docs/decisions/0006-managed-abi.md
// and 0011), so every entry registered here goes through GESHER_JNI, the
// System V thunk from core_jni_helpers.h (frameworks/base patch 0001).

#include <gesher/builtin_library.h>

#include <time.h>

#include <android_runtime/AndroidRuntime.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {

// The register_* functions of the frameworks/base sources built so far
// (cmake/aosp/framework.cmake); each dies on failure.
int register_android_os_MessageQueue(JNIEnv* env);
int register_android_os_SystemClock(JNIEnv* env);
int register_android_os_SystemProperties(JNIEnv* env);
int register_android_os_Trace(JNIEnv* env);
int register_android_text_AndroidCharacter(JNIEnv* env);
int register_android_util_EventLog(JNIEnv* env);
int register_android_util_Log(JNIEnv* env);
int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv* env);

// --- AndroidRuntime statics ---------------------------------------------------
// There is no AndroidRuntime instance (no Zygote; ART is started by dalvikvm
// or gesher-app); the statics the framework's natives call are defined here.

static JavaVM* gGesherJavaVM = nullptr;

/* static */ JavaVM* AndroidRuntime::getJavaVM() {
  return gGesherJavaVM;
}

/* static */ JNIEnv* AndroidRuntime::getJNIEnv() {
  JNIEnv* env = nullptr;
  if (gGesherJavaVM == nullptr ||
      gGesherJavaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK) {
    return nullptr;
  }
  return env;
}

/* static */ int AndroidRuntime::registerNativeMethods(JNIEnv* env, const char* className,
                                                       const JNINativeMethod* gMethods,
                                                       int numMethods) {
  return jniRegisterNativeMethods(env, className, gMethods, numMethods);
}

// --- android.app.Activity (Gesher stub) ---------------------------------------
// The real android_app_Activity.cpp asks the Android linker for its dlopen
// warning (android_dlwarning); there is none here.

static jstring Activity_getDlWarning(JNIEnv*, jobject) {
  return nullptr;
}

static const JNINativeMethod gActivityMethods[] = {
    {"getDlWarning", "()Ljava/lang/String;", GESHER_JNI(Activity_getDlWarning)},
};

// --- android.os.Process (Gesher stub) ------------------------------------------
// The smallest set the headless framework calls: thread priority (accepted
// and ignored for now), argv0 and CPU time. Grown as UnsatisfiedLinkErrors
// appear (docs/framework-natives.md).

static void Process_setThreadPriority(JNIEnv*, jclass, jint, jint) {}

static void Process_setCallingThreadPriority(JNIEnv*, jclass, jint) {}

static jint Process_getThreadPriority(JNIEnv*, jclass, jint) {
  return 0;  // THREAD_PRIORITY_DEFAULT
}

static void Process_setArgV0(JNIEnv*, jclass, jstring) {}

static jlong Process_getElapsedCpuTime(JNIEnv*, jclass) {
  struct timespec ts;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static const JNINativeMethod gProcessMethods[] = {
    {"setThreadPriority", "(II)V", GESHER_JNI(Process_setThreadPriority)},
    {"setThreadPriority", "(I)V", GESHER_JNI(Process_setCallingThreadPriority)},
    {"getThreadPriority", "(I)I", GESHER_JNI(Process_getThreadPriority)},
    {"setArgV0", "(Ljava/lang/String;)V", GESHER_JNI(Process_setArgV0)},
    {"getElapsedCpuTime", "()J", GESHER_JNI(Process_getElapsedCpuTime)},
};

// --- gesher.NativeCanary --------------------------------------------------------
// @CriticalNative ABI canaries for the managed-code boundary: enough integer
// and floating-point arguments that a System V / Microsoft calling-convention
// mismatch (register count, stack layout) changes the result or crashes.
// CRITICAL_JNI_PARAMS is empty under GESHER (core_jni_helpers.h), as ART
// honours @CriticalNative. Registered only when the class is on the class
// path (it lives in the test dex, not in the framework).

static jlong NativeCanary_ints(jint a, jint b, jint c, jint d, jint e, jint f, jint g, jint h,
                               jlong i, jlong j) {
  return a + 3LL * b + 5LL * c + 7LL * d + 11LL * e + 13LL * f + 17LL * g + 19LL * h + 23LL * i +
         29LL * j;
}

static jdouble NativeCanary_floats(jfloat a, jdouble b, jfloat c, jdouble d, jfloat e, jdouble f,
                                   jfloat g, jdouble h, jfloat i) {
  return a + 3.0 * b + 5.0 * c + 7.0 * d + 11.0 * e + 13.0 * f + 17.0 * g + 19.0 * h + 23.0 * i;
}

static jdouble NativeCanary_mixed(jint a, jfloat b, jlong c, jdouble d, jint e, jfloat f, jlong g,
                                  jdouble h) {
  return a + 3.0 * b + 5.0 * c + 7.0 * d + 11.0 * e + 13.0 * f + 17.0 * g + 19.0 * h;
}

static const JNINativeMethod gCanaryMethods[] = {
    {"ints", "(IIIIIIIIJJ)J", GESHER_JNI(NativeCanary_ints)},
    {"floats", "(FDFDFDFDF)D", GESHER_JNI(NativeCanary_floats)},
    {"mixed", "(IFJDIFJD)D", GESHER_JNI(NativeCanary_mixed)},
};

// --- registration ---------------------------------------------------------------

static int register_android_app_Activity(JNIEnv* env) {
  return RegisterMethodsOrDie(env, "android/app/Activity", gActivityMethods,
                              NELEM(gActivityMethods));
}

static int register_android_os_Process(JNIEnv* env) {
  return RegisterMethodsOrDie(env, "android/os/Process", gProcessMethods, NELEM(gProcessMethods));
}

// The canary class exists only in the test dex; with the real framework on
// the class path there is nothing to register on, which is not an error.
static int register_gesher_NativeCanary(JNIEnv* env) {
  jclass clazz = env->FindClass("gesher/NativeCanary");
  if (clazz == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  return RegisterMethodsOrDie(env, "gesher/NativeCanary", gCanaryMethods, NELEM(gCanaryMethods));
}

static const struct {
  const char* name;
  int (*registerFunction)(JNIEnv*);
} gRegJNI[] = {
    {"android_os_MessageQueue", register_android_os_MessageQueue},
    {"android_os_SystemClock", register_android_os_SystemClock},
    {"android_os_SystemProperties", register_android_os_SystemProperties},
    {"android_os_Trace", register_android_os_Trace},
    {"android_text_AndroidCharacter", register_android_text_AndroidCharacter},
    {"android_util_EventLog", register_android_util_EventLog},
    {"android_util_Log", register_android_util_Log},
    {"com_android_internal_util_VirtualRefBasePtr",
     register_com_android_internal_util_VirtualRefBasePtr},
    {"android_app_Activity", register_android_app_Activity},
    {"android_os_Process", register_android_os_Process},
    {"gesher_NativeCanary", register_gesher_NativeCanary},
};

}  // namespace android

extern "C" jint JNI_OnLoad_android_runtime(JavaVM* vm, void*) {
  android::gGesherJavaVM = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  for (const auto& r : android::gRegJNI) {
    if (r.registerFunction(env) < 0) {
      ALOGE("libandroid_runtime: registering %s failed", r.name);
      return JNI_ERR;
    }
  }
  return JNI_VERSION_1_6;
}

namespace {

const gesher_builtin_symbol kAndroidRuntime[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_android_runtime)},
};

[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("libandroid_runtime.so", kAndroidRuntime,
                                  sizeof(kAndroidRuntime) / sizeof(kAndroidRuntime[0]));
  return true;
}();

}  // namespace
