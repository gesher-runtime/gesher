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

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <android_runtime/AndroidRuntime.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <utils/AndroidThreads.h>

#include "core_jni_helpers.h"

#include "gesher_device_standins.h"

// android_util_Binder.cpp defines its register function outside the namespace,
// as compat/hwui/graphics_runtime.cc does.
int register_android_os_Binder(JNIEnv* env);
// hwui's natives (android.graphics, RenderNode, HardwareRenderer) and
// Surface's: compat/hwui/graphics_runtime.cc, docs/graphics.md.
int register_gesher_graphics(JNIEnv* env);
int register_gesher_attach_standins(JNIEnv* env);

namespace android {

// The register_* functions of the frameworks/base sources built so far
// (cmake/aosp/framework.cmake); each dies on failure.
int register_android_content_AssetManager(JNIEnv* env);
int register_android_content_res_ApkAssets(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
int register_android_os_MessageQueue(JNIEnv* env);
int register_android_os_Parcel(JNIEnv* env);
int register_android_os_ServiceManagerNative(JNIEnv* env);
int register_android_os_SystemClock(JNIEnv* env);
int register_android_os_SystemProperties(JNIEnv* env);
int register_android_os_Trace(JNIEnv* env);
int register_android_text_AndroidCharacter(JNIEnv* env);
int register_android_util_EventLog(JNIEnv* env);
int register_android_util_Log(JNIEnv* env);
int register_android_util_jar_StrictJarFile(JNIEnv* env);
int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv* env);
// services_natives.cc: the app process attached to a system server.
int register_gesher_ApplicationSharedMemory(JNIEnv* env);
int register_gesher_ClassLoaderFactory(JNIEnv* env);
int register_gesher_ActivityThread(JNIEnv* env);
int register_gesher_DebugStore(JNIEnv* env);
int register_gesher_PropertyInvalidatedCache(JNIEnv* env);  // services_natives_pic.cc

// libutils' androidSetThreadName, which libbinder's ProcessState names its
// pooled threads with, comes from libutils' pthread Threads.cpp (system/core
// 0003, docs/core-libs.md).

// --- AndroidRuntime statics ---------------------------------------------------
// There is no AndroidRuntime instance (no Zygote; ART is started by dalvikvm
// or gesher-app); the statics the framework's natives call are defined here.

static JavaVM* gGesherJavaVM = nullptr;

/* static */ JavaVM* AndroidRuntime::getJavaVM() {
  return gGesherJavaVM;
}

// --- threads that can call Java ----------------------------------------------------
// AndroidRuntime::startReg's androidSetCreateThreadFunc(javaCreateThreadEtc):
// threads libutils creates for Java (libbinder's pooled binder threads,
// which run Java Binders' onTransact) are attached to the VM for their
// lifetime, as AndroidRuntime.cpp's javaThreadShell does.

struct JavaThreadStart {
  android_thread_func_t entry;
  void* user_data;
  char* name;
};

static int JavaThreadShell(void* arg) {
  JavaThreadStart start = *static_cast<JavaThreadStart*>(arg);
  delete static_cast<JavaThreadStart*>(arg);
  JavaVMAttachArgs args = {JNI_VERSION_1_4, start.name, nullptr};
  JNIEnv* env = nullptr;
  if (gGesherJavaVM->AttachCurrentThread(&env, &args) != JNI_OK) {
    ALOGE("libandroid_runtime: attaching thread %s failed", start.name);
    free(start.name);
    return -1;
  }
  int result = start.entry(start.user_data);
  gGesherJavaVM->DetachCurrentThread();
  free(start.name);
  return result;
}

static int JavaCreateThreadEtc(android_thread_func_t entry, void* user_data,
                               const char* thread_name, int32_t thread_priority,
                               size_t thread_stack_size, android_thread_id_t* thread_id) {
  LOG_ALWAYS_FATAL_IF(thread_name == nullptr, "threadName not provided to javaCreateThreadEtc");
  auto* start = new JavaThreadStart{entry, user_data, strdup(thread_name)};
  return androidCreateRawThreadEtc(JavaThreadShell, start, thread_name, thread_priority,
                                   thread_stack_size, thread_id);
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
// The real android_util_Process.cpp is cgroups, /proc and libprocessgroup.
// This is the part of its table an app process calls, by the same names and
// signatures: thread priority and groups (accepted and ignored: there are no
// cgroups, and Windows priorities are ART's business), argv0, CPU time,
// signals (gesher-base's kill), and name lookups (no users or groups). Grown
// as UnsatisfiedLinkErrors appear (docs/framework-natives.md).

static void Process_setThreadPriorityNative(JNIEnv*, jclass, jint, jint) {}

static jint Process_getThreadPriority(JNIEnv*, jclass, jint) {
  return 0;  // THREAD_PRIORITY_DEFAULT
}

static void Process_setThreadGroup(JNIEnv*, jclass, jint, jint) {}

static jint Process_getProcessGroup(JNIEnv*, jclass, jint) {
  return -1;  // THREAD_GROUP_DEFAULT
}

static void Process_setArgV0Native(JNIEnv*, jclass, jstring) {}

static jlong Process_getElapsedCpuTime(JNIEnv*, jclass) {
  struct timespec ts;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
  return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static jint Process_getIdForName(JNIEnv*, jclass, jstring) {
  return -1;
}

static void Process_sendSignal(JNIEnv*, jclass, jint pid, jint sig) {
  if (pid > 0) kill(pid, sig);
}

static const JNINativeMethod gProcessMethods[] = {
    {"setThreadPriorityNative", "(II)V", GESHER_JNI(Process_setThreadPriorityNative)},
    {"getThreadPriority", "(I)I", GESHER_JNI(Process_getThreadPriority)},
    {"setThreadGroup", "(II)V", GESHER_JNI(Process_setThreadGroup)},
    {"setThreadGroupAndCpuset", "(II)V", GESHER_JNI(Process_setThreadGroup)},
    {"setProcessGroup", "(II)V", GESHER_JNI(Process_setThreadGroup)},
    {"getProcessGroup", "(I)I", GESHER_JNI(Process_getProcessGroup)},
    {"setArgV0Native", "(Ljava/lang/String;)V", GESHER_JNI(Process_setArgV0Native)},
    {"getElapsedCpuTime", "()J", GESHER_JNI(Process_getElapsedCpuTime)},
    {"getUidForName", "(Ljava/lang/String;)I", GESHER_JNI(Process_getIdForName)},
    {"getGidForName", "(Ljava/lang/String;)I", GESHER_JNI(Process_getIdForName)},
    {"sendSignal", "(II)V", GESHER_JNI(Process_sendSignal)},
    {"sendSignalQuiet", "(II)V", GESHER_JNI(Process_sendSignal)},
};

// --- android.os.PerfettoTrace (Gesher stub) ---------------------------------------
// The real android_os_PerfettoTrace.cpp is the perfetto SDK, which Gesher does
// not build (tracing_perfetto_stub.cc). These natives keep the Java side
// working with tracing off: categories exist but are never enabled, and
// sessions record nothing. MessageQueue builds PerfettoTrace's categories on
// its first message.

static void PerfettoCategory_free(void* ptr) {
  free(ptr);
}

static jlong PerfettoCategory_init(JNIEnv*, jclass, jstring, jstring, jstring) {
  return reinterpret_cast<jlong>(calloc(1, 16));
}

static jlong PerfettoCategory_delete() {
  return reinterpret_cast<jlong>(&PerfettoCategory_free);
}

static void PerfettoCategory_register(jlong) {}

static jboolean PerfettoCategory_isEnabled(jlong) {
  return JNI_FALSE;
}

static jlong PerfettoCategory_getExtraPtr(jlong) {
  return 0;
}

static const JNINativeMethod gPerfettoCategoryMethods[] = {
    {"native_init", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J",
     GESHER_JNI(PerfettoCategory_init)},
    {"native_delete", "()J", GESHER_JNI(PerfettoCategory_delete)},
    {"native_register", "(J)V", GESHER_JNI(PerfettoCategory_register)},
    {"native_unregister", "(J)V", GESHER_JNI(PerfettoCategory_register)},
    {"native_is_enabled", "(J)Z", GESHER_JNI(PerfettoCategory_isEnabled)},
    {"native_get_extra_ptr", "(J)J", GESHER_JNI(PerfettoCategory_getExtraPtr)},
};

static jlong PerfettoTrace_getProcessTrackUuid() {
  return 0;
}

static jlong PerfettoTrace_getThreadTrackUuid(jlong) {
  return 0;
}

static void PerfettoTrace_activateTrigger(JNIEnv*, jclass, jstring, jint) {}

static void PerfettoTrace_register(JNIEnv*, jclass, jboolean) {}

static jlong PerfettoTrace_startSession(JNIEnv*, jclass, jboolean, jbyteArray) {
  return 0;
}

static jbyteArray PerfettoTrace_stopSession(JNIEnv* env, jclass, jlong) {
  return env->NewByteArray(0);
}

static const JNINativeMethod gPerfettoTraceMethods[] = {
    {"native_get_process_track_uuid", "()J", GESHER_JNI(PerfettoTrace_getProcessTrackUuid)},
    {"native_get_thread_track_uuid", "(J)J", GESHER_JNI(PerfettoTrace_getThreadTrackUuid)},
    {"native_activate_trigger", "(Ljava/lang/String;I)V",
     GESHER_JNI(PerfettoTrace_activateTrigger)},
    {"native_register", "(Z)V", GESHER_JNI(PerfettoTrace_register)},
    {"native_start_session", "(Z[B)J", GESHER_JNI(PerfettoTrace_startSession)},
    {"native_stop_session", "(J)[B", GESHER_JNI(PerfettoTrace_stopSession)},
};

static int register_android_os_PerfettoTrace(JNIEnv* env) {
  RegisterMethodsOrDie(env, "android/os/PerfettoTrace", gPerfettoTraceMethods,
                       NELEM(gPerfettoTraceMethods));
  return RegisterMethodsOrDie(env, "android/os/PerfettoTrace$Category", gPerfettoCategoryMethods,
                              NELEM(gPerfettoCategoryMethods));
}

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

static int register_gesher_NativeCanary(JNIEnv* env) {
  return RegisterMethodsOrDie(env, "gesher/NativeCanary", gCanaryMethods, NELEM(gCanaryMethods));
}

// Each registration with a class it needs. When that class is not on the
// class path the registration is skipped, with a log line: the framework's
// register functions abort when their classes are missing, which with
// framework.jar never happens, but the tests load stand-ins for a few classes
// only (tests/dex-natives), and the canary class is never in the framework.
// Binder and Parcel need BinderInternal, which no stand-in has: the stand-ins
// for Binder and Parcel (what Looper and Message use) carry no natives.
static const struct {
  const char* name;
  const char* className;
  int (*registerFunction)(JNIEnv*);
} gRegJNI[] = {
    // AndroidRuntime.cpp's order: the logging, clock and property natives
    // first, as other registrations initialize classes (StrictMode, from
    // Binder's) whose static initializers call them.
    {"android_os_SystemClock", "android/os/SystemClock", register_android_os_SystemClock},
    {"android_util_EventLog", "android/util/EventLog", register_android_util_EventLog},
    {"android_util_Log", "android/util/Log", register_android_util_Log},
    {"android_os_SystemProperties", "android/os/SystemProperties",
     register_android_os_SystemProperties},
    {"android_os_Trace", "android/os/Trace", register_android_os_Trace},
    // Needs a class the tests' PerfettoTrace stand-in lacks.
    {"android_os_PerfettoTrace", "android/os/PerfettoTrackEventExtra",
     register_android_os_PerfettoTrace},
    {"android_os_Process", "android/os/Process", register_android_os_Process},
    {"android_text_AndroidCharacter", "android/text/AndroidCharacter",
     register_android_text_AndroidCharacter},
    {"com_android_internal_util_VirtualRefBasePtr", "com/android/internal/util/VirtualRefBasePtr",
     register_com_android_internal_util_VirtualRefBasePtr},
    {"android_util_jar_StrictJarFile", "android/util/jar/StrictJarFile",
     register_android_util_jar_StrictJarFile},
    {"android_os_MessageQueue", "android/os/MessageQueue", register_android_os_MessageQueue},
    {"android_os_Binder", "com/android/internal/os/BinderInternal", ::register_android_os_Binder},
    {"android_os_Parcel", "com/android/internal/os/BinderInternal", register_android_os_Parcel},
    {"android_os_ServiceManagerNative", "android/os/ServiceManagerProxy",
     register_android_os_ServiceManagerNative},
    {"android_content_res_ApkAssets", "android/content/res/ApkAssets",
     register_android_content_res_ApkAssets},
    {"android_content_AssetManager", "android/content/res/AssetManager",
     register_android_content_AssetManager},
    {"android_content_StringBlock", "android/content/res/StringBlock",
     register_android_content_StringBlock},
    {"android_content_XmlBlock", "android/content/res/XmlBlock",
     register_android_content_XmlBlock},
    // Stand-ins for graphics and view natives on the attached app process's path
    // (attach_standins.cc); before the graphics rows, so real natives replace them.
    {"gesher_attach_standins", "android/graphics/Canvas", ::register_gesher_attach_standins},
    // hwui (AndroidRuntime.cpp registers it through libhwui's
    // register_android_graphics_classes), then the device-only stand-ins.
    {"gesher_graphics", "android/graphics/RenderNode", ::register_gesher_graphics},
    {"gesher_device_standins", "android/view/SurfaceControl", ::register_gesher_device_standins},
    {"android_app_Activity", "android/app/Activity", register_android_app_Activity},
    {"gesher_ApplicationSharedMemory", "com/android/internal/os/ApplicationSharedMemory",
     register_gesher_ApplicationSharedMemory},
    {"gesher_ClassLoaderFactory", "com/android/internal/os/ClassLoaderFactory",
     register_gesher_ClassLoaderFactory},
    {"gesher_ActivityThread", "android/app/ActivityThread", register_gesher_ActivityThread},
    {"gesher_DebugStore", "com/android/internal/os/DebugStore", register_gesher_DebugStore},
    {"gesher_PropertyInvalidatedCache", "android/app/PropertyInvalidatedCache",
     register_gesher_PropertyInvalidatedCache},
    {"gesher_NativeCanary", "gesher/NativeCanary", register_gesher_NativeCanary},
};

}  // namespace android

extern "C" jint JNI_OnLoad_android_runtime(JavaVM* vm, void*) {
  android::gGesherJavaVM = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  androidSetCreateThreadFunc(android::JavaCreateThreadEtc);
  for (const auto& r : android::gRegJNI) {
    jclass clazz = env->FindClass(r.className);
    if (clazz == nullptr) {
      env->ExceptionClear();
      ALOGI("libandroid_runtime: %s not on the class path; %s skipped", r.className, r.name);
      continue;
    }
    env->DeleteLocalRef(clazz);
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
