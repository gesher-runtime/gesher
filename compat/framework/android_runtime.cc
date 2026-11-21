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
#include <gesher/process_name.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <windows.h>

#include <string>
#include <vector>

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
// The Java OpenGL ES/EGL bindings (EGL10/GL10, android.opengl) over ANGLE:
// compat/opengl/opengl_jni.cc, docs/angle.md.
int register_gesher_opengl(JNIEnv* env);
int register_gesher_attach_standins(JNIEnv* env);
// libandroid_runtime's input natives (InputChannel, InputEventReceiver/Sender,
// KeyEvent, MotionEvent, KeyCharacterMap, VelocityTracker) and gesher-app's
// input classes: compat/input/input_jni.cc, docs/input.md.
int register_gesher_input(JNIEnv* env);
// android.os.SharedMemory over memfds: compat/framework/shared_memory.cc.
int register_gesher_shared_memory(JNIEnv* env);
// android.app.NativeActivity's and android.view.InputQueue's natives for an
// app's ELF libraries: compat/bionic/native_activity.cc, docs/native-libs.md.
int register_gesher_native_activity(JNIEnv* env);

namespace android {

// The register_* functions of the frameworks/base sources built so far
// (cmake/aosp/framework.cmake); each dies on failure.
int register_android_content_AssetManager(JNIEnv* env);
int register_android_content_res_ApkAssets(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
int register_android_os_FileObserver(JNIEnv* env);
int register_android_os_MessageQueue(JNIEnv* env);
int register_gesher_MemoryFile(JNIEnv* env);
int register_gesher_MemoryIntArray(JNIEnv* env);
int register_gesher_Scrypt(JNIEnv* env);
int register_gesher_LocalSocketImpl(JNIEnv* env);
int register_gesher_CharsetUtils(JNIEnv* env);
int register_android_os_Parcel(JNIEnv* env);
int register_android_os_ServiceManagerNative(JNIEnv* env);
int register_android_os_SystemClock(JNIEnv* env);
int register_android_os_SystemProperties(JNIEnv* env);
int register_android_os_Trace(JNIEnv* env);
int register_android_text_AndroidCharacter(JNIEnv* env);
int register_android_util_EventLog(JNIEnv* env);
int register_android_util_Log(JNIEnv* env);
int register_android_util_jar_StrictJarFile(JNIEnv* env);
int register_android_animation_PropertyValuesHolder(JNIEnv* env);
int register_android_database_CursorWindow(JNIEnv* env);
int register_android_database_SQLiteConnection(JNIEnv* env);
int register_android_database_SQLiteDebug(JNIEnv* env);
int register_android_database_SQLiteGlobal(JNIEnv* env);
int register_android_database_SQLiteRawStatement(JNIEnv* env);
int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv* env);
// services_natives.cc: the app process attached to a system server.
int register_gesher_ApplicationSharedMemory(JNIEnv* env);
int register_gesher_ClassLoaderFactory(JNIEnv* env);
int register_gesher_ActivityThread(JNIEnv* env);
int register_gesher_DebugStore(JNIEnv* env);
int register_gesher_PropertyInvalidatedCache(JNIEnv* env);  // services_natives_pic.cc
int register_gesher_Zygote(JNIEnv* env);  // services_natives.cc, services_zygote.cc
int register_gesher_Shell(JNIEnv* env);  // compat/shell/shell_natives.cc (shell track)
int register_gesher_media(JNIEnv* env);  // compat/media/jni (media track, docs/media.md)
int register_gesher_surface_parcel(JNIEnv* env);  // compat/framework/surface_parcel.cc
int register_gesher_devices(JNIEnv* env);  // compat/devices/devices_jni.cc (devices track)
int register_gesher_WindowsHost(JNIEnv* env);  // compat/devsvc/devsvc_natives.cc (devsvc track)

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

// As AndroidRuntime::setArgv0(name, /*setProcName=*/true) on a device: the process
// name /proc/self/cmdline and /proc/self/comm report (gesher/process_name.h), and
// the calling thread's name, shortened by libutils' Android naming rules.
static void Process_setArgV0Native(JNIEnv* env, jclass, jstring name) {
  if (name == nullptr) return;
  const char* utf = env->GetStringUTFChars(name, nullptr);
  if (utf == nullptr) return;
  gesher_set_process_argv0(utf);
  androidSetThreadName(utf);
  env->ReleaseStringUTFChars(name, utf);
}

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

// Match pinned android_util_Process.cpp: numeric line prefixes, zero missing
// fields after a successful open, and preserve the output when open fails.
static void Process_readProcLines(JNIEnv* env, jclass, jstring file,
                                  jobjectArray requested, jlongArray output) {
  if (file == nullptr || requested == nullptr || output == nullptr) {
    jniThrowNullPointerException(env, nullptr);
    return;
  }
  jsize count = env->GetArrayLength(requested);
  if (count > env->GetArrayLength(output)) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "Array lengths differ");
    return;
  }
  const char* path = env->GetStringUTFChars(file, nullptr);
  if (path == nullptr) return;
  std::string filename(path);
  env->ReleaseStringUTFChars(file, path);
  std::vector<std::string> fields;
  for (jsize i = 0; i < count; ++i) {
    auto field = static_cast<jstring>(env->GetObjectArrayElement(requested, i));
    if (field == nullptr) {
      jniThrowNullPointerException(env, "Element in reqFields");
      return;
    }
    const char* value = env->GetStringUTFChars(field, nullptr);
    if (value == nullptr) {
      env->DeleteLocalRef(field);
      return;
    }
    fields.emplace_back(value);
    env->ReleaseStringUTFChars(field, value);
    env->DeleteLocalRef(field);
  }
  jlong* values = env->GetLongArrayElements(output, nullptr);
  if (values == nullptr) return;
  int fd = open(filename.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    for (jsize i = 0; i < count; ++i) values[i] = 0;
    char buffer[4096];
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length < 0) length = 0;
    buffer[length] = 0;
    jsize found = 0;
    for (char* line = buffer; *line != 0 && found < count;) {
      char* end = strchr(line, '\n');
      if (end != nullptr) *end = 0;
      for (jsize i = 0; i < count; ++i) {
        if (strncmp(line, fields[i].c_str(), fields[i].size()) != 0) continue;
        char* number = line + fields[i].size();
        while (*number == ' ' || *number == '\t') ++number;
        char* last = number;
        while (*last >= '0' && *last <= '9') ++last;
        *last = 0;
        values[i] = strtoll(number, nullptr, 10);
        ++found;
        break;
      }
      if (end == nullptr) break;
      line = end + 1;
    }
  }
  env->ReleaseLongArrayElements(output, values, 0);
}

enum {
  PROC_TERM_MASK = 0xff,
  PROC_COMBINE = 0x100,
  PROC_PARENS = 0x200,
  PROC_QUOTES = 0x400,
  PROC_CHAR = 0x800,
  PROC_OUT_STRING = 0x1000,
  PROC_OUT_LONG = 0x2000,
  PROC_OUT_FLOAT = 0x4000,
};

// android-16.0.0_r4's field format has one shared output index, including
// fields written to more than one output array. A missing later field leaves
// earlier results intact and returns false. The buffer has an extra sentinel.
static jboolean Process_parseProcBuffer(JNIEnv* env, char* buffer, jint start,
                                        jint limit, jintArray format,
                                        jobjectArray strings, jlongArray longs,
                                        jfloatArray floats) {
  jsize count = env->GetArrayLength(format);
  std::vector<jint> modes(count);
  if (count > 0) env->GetIntArrayRegion(format, 0, count, modes.data());
  if (env->ExceptionCheck()) return JNI_FALSE;
  jsize string_count = strings == nullptr ? 0 : env->GetArrayLength(strings);
  jsize long_count = longs == nullptr ? 0 : env->GetArrayLength(longs);
  jsize float_count = floats == nullptr ? 0 : env->GetArrayLength(floats);
  jsize dest = 0;
  jint i = start;
  for (jint mode : modes) {
    if (i >= limit) return JNI_FALSE;
    if ((mode & PROC_PARENS) != 0) {
      ++i;
    } else if ((mode & PROC_QUOTES) != 0) {
      if (buffer[i] == '"')
        ++i;
      else
        mode &= ~PROC_QUOTES;
    }
    jint field_start = i;
    if (i >= limit) return JNI_FALSE;
    jint end = -1;
    if ((mode & (PROC_PARENS | PROC_QUOTES)) != 0) {
      char close = (mode & PROC_PARENS) != 0 ? ')' : '"';
      while (i < limit && buffer[i] != close) ++i;
      end = i;
      if (i < limit) ++i;
    }
    char term = static_cast<char>(mode & PROC_TERM_MASK);
    while (i < limit && buffer[i] != term) ++i;
    if (end < 0) end = i;
    if (i < limit) {
      ++i;
      if ((mode & PROC_COMBINE) != 0) {
        while (i < limit && buffer[i] == term) ++i;
      }
    }
    if ((mode & (PROC_OUT_STRING | PROC_OUT_LONG | PROC_OUT_FLOAT)) == 0)
      continue;
    char saved = buffer[end];
    buffer[end] = 0;
    if ((mode & PROC_OUT_FLOAT) != 0 && dest < float_count) {
      jfloat value = strtof(buffer + field_start, nullptr);
      env->SetFloatArrayRegion(floats, dest, 1, &value);
    }
    if ((mode & PROC_OUT_LONG) != 0 && dest < long_count) {
      jlong value = (mode & PROC_CHAR) != 0
                        ? buffer[field_start]
                        : strtoll(buffer + field_start, nullptr, 10);
      env->SetLongArrayRegion(longs, dest, 1, &value);
    }
    if ((mode & PROC_OUT_STRING) != 0 && dest < string_count) {
      for (jint j = field_start; j < end; ++j) {
        if (!isprint(static_cast<unsigned char>(buffer[j]))) buffer[j] = '?';
      }
      jstring value = env->NewStringUTF(buffer + field_start);
      if (value != nullptr) {
        env->SetObjectArrayElement(strings, dest, value);
        env->DeleteLocalRef(value);
      }
    }
    buffer[end] = saved;
    if (env->ExceptionCheck()) return JNI_FALSE;
    ++dest;
  }
  return JNI_TRUE;
}

static jboolean Process_parseProcLine(JNIEnv* env, jclass, jbyteArray input,
                                      jint start, jint end, jintArray format,
                                      jobjectArray strings, jlongArray longs,
                                      jfloatArray floats) {
  if (input == nullptr || format == nullptr) {
    jniThrowNullPointerException(env, nullptr);
    return JNI_FALSE;
  }
  jsize length = env->GetArrayLength(input);
  if (start < 0 || end < start || end > length) {
    jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException",
                      "Invalid proc buffer range");
    return JNI_FALSE;
  }
  std::vector<char> buffer(static_cast<size_t>(end) + 1);
  env->GetByteArrayRegion(input, 0, end,
                          reinterpret_cast<jbyte*>(buffer.data()));
  if (env->ExceptionCheck()) return JNI_FALSE;
  jboolean result = Process_parseProcBuffer(env, buffer.data(), start, end,
                                            format, strings, longs, floats);
  // The pinned parser also writes sanitized string bytes back to the input.
  if (!env->ExceptionCheck()) {
    env->SetByteArrayRegion(
        input, start, end - start,
        reinterpret_cast<const jbyte*>(buffer.data() + start));
  }
  return result;
}

static jboolean Process_readProcFile(JNIEnv* env, jclass, jstring file,
                                     jintArray format, jobjectArray strings,
                                     jlongArray longs, jfloatArray floats) {
  if (file == nullptr || format == nullptr) {
    jniThrowNullPointerException(env, nullptr);
    return JNI_FALSE;
  }
  const char* path = env->GetStringUTFChars(file, nullptr);
  if (path == nullptr) return JNI_FALSE;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  env->ReleaseStringUTFChars(file, path);
  if (fd < 0) return JNI_FALSE;
  constexpr size_t kMaxBytes = 64 << 20;
  size_t capacity = 1024;
  size_t length = 0;
  std::vector<char> buffer(capacity + 1);
  for (;;) {
    ssize_t n = pread(fd, buffer.data() + length, capacity - length, length);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      close(fd);
      return JNI_FALSE;
    }
    if (n == 0) break;
    length += n;
    if (length == capacity) {
      if (capacity >= kMaxBytes) {
        close(fd);
        return JNI_FALSE;
      }
      capacity *= 2;
      buffer.resize(capacity + 1);
    }
  }
  close(fd);
  buffer[length] = 0;
  return Process_parseProcBuffer(env, buffer.data(), 0,
                                 static_cast<jint>(length), format, strings,
                                 longs, floats);
}

static const JNINativeMethod gProcessMethods[] = {
    {"setThreadPriorityNative", "(II)V",
     GESHER_JNI(Process_setThreadPriorityNative)},
    {"getThreadPriority", "(I)I", GESHER_JNI(Process_getThreadPriority)},
    {"setThreadGroup", "(II)V", GESHER_JNI(Process_setThreadGroup)},
    {"setThreadGroupAndCpuset", "(II)V", GESHER_JNI(Process_setThreadGroup)},
    {"setProcessGroup", "(II)V", GESHER_JNI(Process_setThreadGroup)},
    {"getProcessGroup", "(I)I", GESHER_JNI(Process_getProcessGroup)},
    {"setArgV0Native", "(Ljava/lang/String;)V",
     GESHER_JNI(Process_setArgV0Native)},
    {"getElapsedCpuTime", "()J", GESHER_JNI(Process_getElapsedCpuTime)},
    {"getUidForName", "(Ljava/lang/String;)I",
     GESHER_JNI(Process_getIdForName)},
    {"getGidForName", "(Ljava/lang/String;)I",
     GESHER_JNI(Process_getIdForName)},
    {"sendSignal", "(II)V", GESHER_JNI(Process_sendSignal)},
    {"sendSignalQuiet", "(II)V", GESHER_JNI(Process_sendSignal)},
    {"readProcLines", "(Ljava/lang/String;[Ljava/lang/String;[J)V",
     GESHER_JNI(Process_readProcLines)},
    {"readProcFile", "(Ljava/lang/String;[I[Ljava/lang/String;[J[F)Z",
     GESHER_JNI(Process_readProcFile)},
    {"parseProcLine", "([BII[I[Ljava/lang/String;[J[F)Z",
     GESHER_JNI(Process_parseProcLine)},
};

// --- android.os.Debug (native heap and Binder counters) ----------------------------
// AOSP's android_os_Debug.cpp answers these @CriticalNative methods from
// bionic's mallinfo(). The host malloc is the UCRT's, over the process heap,
// so HeapSummary (kernel32, Windows 10) gives the same three numbers: the
// heap's committed bytes, the bytes allocated in it and the rest. Apps poll
// them for memory metrics (Waze's startup coroutine calls getNativeHeapSize
// and dies of the UnsatisfiedLinkError when it is unregistered).

namespace {

struct NativeHeap {
  jlong size = 0;
  jlong allocated = 0;
  jlong available = 0;
};

NativeHeap NativeHeapSummary() {
  struct Summary {  // heapapi.h's HEAP_SUMMARY
    DWORD cb;
    SIZE_T allocated;
    SIZE_T committed;
    SIZE_T reserved;
    SIZE_T max_reserve;
  };
  using HeapSummaryFn = BOOL(WINAPI*)(HANDLE, DWORD, Summary*);
  static const HeapSummaryFn heap_summary = reinterpret_cast<HeapSummaryFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary")));
  NativeHeap heap;
  Summary summary = {};
  summary.cb = sizeof(summary);
  if (heap_summary == nullptr || !heap_summary(GetProcessHeap(), 0, &summary)) return heap;
  heap.size = static_cast<jlong>(summary.committed);
  heap.allocated = static_cast<jlong>(summary.allocated);
  heap.available = summary.committed > summary.allocated
                  ? static_cast<jlong>(summary.committed - summary.allocated)
                  : 0;
  return heap;
}

}  // namespace

static jlong Debug_getNativeHeapSize() { return NativeHeapSummary().size; }

static jlong Debug_getNativeHeapAllocatedSize() { return NativeHeapSummary().allocated; }

static jlong Debug_getNativeHeapFreeSize() { return NativeHeapSummary().available; }

// These AOSP functions are already built with android_util_Binder.cpp. They
// report the real JavaBBinder, BpBinder and JavaDeathRecipient object counts;
// OsmAnd's diagnostic worker calls them while sampling process memory.
jint android_os_Debug_getLocalObjectCount(JNIEnv* env, jobject clazz);
jint android_os_Debug_getProxyObjectCount(JNIEnv* env, jobject clazz);
jint android_os_Debug_getDeathObjectCount(JNIEnv* env, jobject clazz);

// AOSP reads transaction totals from the kernel's Binder stats file and returns
// -1 when that file is unavailable. Gesher's broker has no such stats interface.
// Preserve that unavailable result, rather than reporting invented zero totals.
static jint Debug_binderTransactionsUnavailable(JNIEnv*, jobject) { return -1; }

// --- android.os.Debug memory accounting (getMemoryInfo, getPss, getRss, getMemInfo) ---
// AOSP reads a process's smaps (meminfo's ProcMemInfo) and /proc/meminfo. The
// Windows equivalents are the process working set (QueryWorkingSet: one entry
// per resident page, with its share count and protection) and the system's
// performance counters (GetPerformanceInfo). Values are kB, as on Android.
//   - RSS: resident pages. USS: resident pages no other process maps (private,
//     or shared with a share count of 1). PSS: USS plus each shared page divided
//     by its share count (Windows caps the count at 7, so PSS errs high for
//     widely shared DLL pages).
//   - Dirty/clean are estimates from page permissions, not an observed dirty
//     bit: writable pages count as dirty, others as clean. Read-only pages
//     can still be private anonymous memory, so this cannot identify AOSP's
//     file-backed swappable PSS; that unsupported field stays 0.
//   - Categories: Windows does not label mappings like "[anon:dalvik-...]".
//     For the calling process the Java heap's live bytes (Runtime totalMemory
//     minus freeMemory) are reported as dalvik and the process heap's
//     allocated bytes (HeapSummary, as getNativeHeapAllocatedSize) as native:
//     these are allocation estimates, not a per-page residency attribution.
//     Each is capped by the estimated private dirty memory left;
//     the rest is "other" (on Windows that includes ART's mapped image and
//     oat files, private clean as on Linux). Another process is reported entirely as
//     "other". otherStats stays zero: there is no per-mapping breakdown.
//   - Swap: Windows pages out to the pagefile, but per-page swap PSS is not
//     available, so swappedOut is 0 and hasSwappedOutPss false, as on an
//     Android device without swap PSS. There is no memtrack HAL: graphics
//     memory is 0.
// A failure to read another process answers as AOSP does when smaps cannot be
// read: false or 0. Apps sample these for diagnostics (OsmAnd's
// OsmAndDiagnosticThread calls getMemoryInfo; an unregistered native killed it
// with UnsatisfiedLinkError, and OsmAnd then reported a crash on every launch).

namespace {

struct WorkingSetUsage {
  jlong rss = 0, pss = 0, uss = 0, swappable_pss = 0;
  jlong private_dirty = 0, private_clean = 0, shared_dirty = 0, shared_clean = 0;
};

struct WorkingSetEntry {  // psapi.h's PSAPI_WORKING_SET_BLOCK
  ULONG_PTR flags;
  unsigned protection() const { return static_cast<unsigned>(flags & 0x1f); }
  unsigned share_count() const { return static_cast<unsigned>((flags >> 5) & 0x7); }
  bool shared() const { return ((flags >> 8) & 0x1) != 0; }
};

using QueryWorkingSetFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);
using GetPerformanceInfoFn = BOOL(WINAPI*)(void*, DWORD);
using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, void*, DWORD);

template <typename Fn>
Fn Kernel32(const char* name) {
  return reinterpret_cast<Fn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name)));
}

// The process to read, or null. Close it with CloseProcess.
HANDLE OpenForMemory(jint pid) {
  if (pid <= 0) return nullptr;
  if (static_cast<DWORD>(pid) == GetCurrentProcessId()) return GetCurrentProcess();
  return OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
}

void CloseProcess(HANDLE process) {
  if (process != nullptr && process != GetCurrentProcess()) CloseHandle(process);
}

bool ReadWorkingSet(HANDLE process, WorkingSetUsage* usage) {
  static const QueryWorkingSetFn query = Kernel32<QueryWorkingSetFn>("K32QueryWorkingSet");
  if (query == nullptr || process == nullptr) return false;
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  if (system.dwPageSize == 0) return false;
  const double page_kb = system.dwPageSize / 1024.0;
  // ULONG_PTR NumberOfEntries, then the entries. The working set may grow
  // between calls: retry with the reported size plus headroom.
  // Sampling must not terminate the VM under memory pressure (-fno-exceptions)
  // or allocate an unbounded snapshot. 64 MiB holds roughly 32 GiB of resident
  // 4-KiB pages on a 64-bit host; larger samples report unavailable.
  constexpr size_t kMaxBytes = 64 * 1024 * 1024;
  constexpr size_t kMaxSlots = kMaxBytes / sizeof(ULONG_PTR);
  static_assert(kMaxBytes <= UINT32_MAX, "QueryWorkingSet takes a DWORD byte size");
  struct Buffer {
    ULONG_PTR* data = nullptr;
    ~Buffer() { free(data); }
  } buffer;
  size_t capacity = 4096;
  buffer.data = static_cast<ULONG_PTR*>(malloc(capacity * sizeof(ULONG_PTR)));
  if (buffer.data == nullptr) return false;
  for (int attempt = 0;; attempt++) {
    DWORD bytes = static_cast<DWORD>(capacity * sizeof(ULONG_PTR));
    buffer.data[0] = 0;
    if (query(process, buffer.data, bytes)) break;
    if (GetLastError() != ERROR_BAD_LENGTH || attempt == 8) return false;
    ULONG_PTR required = buffer.data[0];
    if (required >= kMaxSlots || required < capacity) return false;
    size_t needed = static_cast<size_t>(required) + 1;  // include NumberOfEntries
    size_t headroom = needed / 4 + 1024;
    size_t next = headroom > kMaxSlots - needed ? kMaxSlots : needed + headroom;
    auto* grown = static_cast<ULONG_PTR*>(realloc(buffer.data, next * sizeof(ULONG_PTR)));
    if (grown == nullptr) return false;
    buffer.data = grown;
    capacity = next;
  }
  size_t count = buffer.data[0];
  if (count >= capacity) return false;  // never report a truncated snapshot as success
  double rss = 0, pss = 0, uss = 0;
  double private_dirty = 0, private_clean = 0, shared_dirty = 0, shared_clean = 0;
  for (size_t i = 0; i < count; i++) {
    WorkingSetEntry page{buffer.data[i + 1]};
    unsigned access = page.protection() & 0x7;
    // 4: read/write, 6: execute/read/write. Copy-on-write protection (5/7)
    // counts as clean in this permission estimate; backing and dirtiness are unknown.
    bool dirty = access == 4 || access == 6;
    bool exclusive = !page.shared() || page.share_count() <= 1;
    double proportional = exclusive ? page_kb : page_kb / page.share_count();
    rss += page_kb;
    pss += proportional;
    if (exclusive) {
      uss += page_kb;
      (dirty ? private_dirty : private_clean) += page_kb;
    } else {
      (dirty ? shared_dirty : shared_clean) += page_kb;
    }
  }
  usage->rss = static_cast<jlong>(rss);
  usage->pss = static_cast<jlong>(pss);
  usage->uss = static_cast<jlong>(uss);
  usage->swappable_pss = 0;  // page permissions cannot establish file-backed clean memory
  usage->private_dirty = static_cast<jlong>(private_dirty);
  usage->private_clean = static_cast<jlong>(private_clean);
  usage->shared_dirty = static_cast<jlong>(shared_dirty);
  usage->shared_clean = static_cast<jlong>(shared_clean);
  return true;
}

// Debug.MemoryInfo's per-category fields (dalvik, native, other), in AOSP's order.
struct CategoryFields {
  jfieldID pss, swappable_pss, rss, private_dirty, shared_dirty, private_clean,
      shared_clean, swapped_out, swapped_out_pss;
};

bool CategoryFieldsFor(JNIEnv* env, jclass type, const char* prefix, CategoryFields* out) {
  const char* names[] = {"Pss", "SwappablePss", "Rss", "PrivateDirty", "SharedDirty",
                         "PrivateClean", "SharedClean", "SwappedOut", "SwappedOutPss"};
  jfieldID* ids[] = {&out->pss, &out->swappable_pss, &out->rss, &out->private_dirty,
                     &out->shared_dirty, &out->private_clean, &out->shared_clean,
                     &out->swapped_out, &out->swapped_out_pss};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    char name[32];
    int length = snprintf(name, sizeof(name), "%s%s", prefix, names[i]);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(name)) return false;
    *ids[i] = env->GetFieldID(type, name, "I");
    if (*ids[i] == nullptr) return false;
  }
  return true;
}

struct Category {
  jlong pss = 0, swappable_pss = 0, rss = 0, private_dirty = 0, shared_dirty = 0,
        private_clean = 0, shared_clean = 0;
};

void SetCategory(JNIEnv* env, jobject info, const CategoryFields& f, const Category& c) {
  auto clamp = [](jlong v) { return static_cast<jint>(v < 0 ? 0 : v > INT32_MAX ? INT32_MAX : v); };
  env->SetIntField(info, f.pss, clamp(c.pss));
  env->SetIntField(info, f.swappable_pss, clamp(c.swappable_pss));
  env->SetIntField(info, f.rss, clamp(c.rss));
  env->SetIntField(info, f.private_dirty, clamp(c.private_dirty));
  env->SetIntField(info, f.shared_dirty, clamp(c.shared_dirty));
  env->SetIntField(info, f.private_clean, clamp(c.private_clean));
  env->SetIntField(info, f.shared_clean, clamp(c.shared_clean));
  env->SetIntField(info, f.swapped_out, 0);
  env->SetIntField(info, f.swapped_out_pss, 0);
}

// The Java heap's live bytes, in kB (Runtime totalMemory - freeMemory), or 0.
jlong JavaHeapKb(JNIEnv* env) {
  jclass runtime = env->FindClass("java/lang/Runtime");
  if (runtime == nullptr) { env->ExceptionClear(); return 0; }
  auto unavailable = [&]() {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(runtime);
    return static_cast<jlong>(0);
  };
  jmethodID get = env->GetStaticMethodID(runtime, "getRuntime", "()Ljava/lang/Runtime;");
  if (get == nullptr) return unavailable();
  jmethodID total = env->GetMethodID(runtime, "totalMemory", "()J");
  if (total == nullptr) return unavailable();
  jmethodID free = env->GetMethodID(runtime, "freeMemory", "()J");
  if (free == nullptr) return unavailable();
  jobject instance = env->CallStaticObjectMethod(runtime, get);
  if (instance == nullptr || env->ExceptionCheck()) {
    env->DeleteLocalRef(instance);
    return unavailable();
  }
  jlong total_bytes = env->CallLongMethod(instance, total);
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(instance);
    return unavailable();
  }
  jlong free_bytes = env->CallLongMethod(instance, free);
  if (env->ExceptionCheck()) {
    env->DeleteLocalRef(instance);
    return unavailable();
  }
  jlong bytes = total_bytes >= 0 && free_bytes >= 0 && free_bytes <= total_bytes
                    ? total_bytes - free_bytes : 0;
  env->DeleteLocalRef(instance);
  env->DeleteLocalRef(runtime);
  return bytes / 1024;
}

}  // namespace

static jboolean Debug_getMemoryInfoPid(JNIEnv* env, jclass, jint pid, jobject info) {
  if (info == nullptr) {
    jniThrowNullPointerException(env, "memoryInfo");
    return JNI_FALSE;
  }
  HANDLE process = OpenForMemory(pid);
  WorkingSetUsage usage;
  bool read = ReadWorkingSet(process, &usage);
  bool self = process == GetCurrentProcess();
  CloseProcess(process);
  if (!read) return JNI_FALSE;

  jclass type = env->FindClass("android/os/Debug$MemoryInfo");
  if (type == nullptr) return JNI_FALSE;
  CategoryFields dalvik_fields, native_fields, other_fields;
  if (!CategoryFieldsFor(env, type, "dalvik", &dalvik_fields)
      || !CategoryFieldsFor(env, type, "native", &native_fields)
      || !CategoryFieldsFor(env, type, "other", &other_fields)) {
    env->DeleteLocalRef(type);
    return JNI_FALSE;  // NoSuchFieldError pending
  }
  jfieldID has_swap_pss = env->GetFieldID(type, "hasSwappedOutPss", "Z");
  if (has_swap_pss == nullptr) {
    env->DeleteLocalRef(type);
    return JNI_FALSE;
  }
  jfieldID other_stats = env->GetFieldID(type, "otherStats", "[I");
  env->DeleteLocalRef(type);
  if (other_stats == nullptr) return JNI_FALSE;

  // Anonymous heaps are private dirty memory: dalvik and native take their
  // share of it, capped by what is resident; the rest of every measure is other.
  Category dalvik, native, other;
  jlong budget = usage.private_dirty;
  if (self) {
    jlong java = JavaHeapKb(env);
    dalvik.pss = dalvik.rss = dalvik.private_dirty = java < budget ? java : budget;
    budget -= dalvik.private_dirty;
    jlong heap = NativeHeapSummary().allocated / 1024;
    native.pss = native.rss = native.private_dirty = heap < budget ? heap : budget;
  }
  other.pss = usage.pss - dalvik.pss - native.pss;
  other.rss = usage.rss - dalvik.rss - native.rss;
  other.private_dirty = usage.private_dirty - dalvik.private_dirty - native.private_dirty;
  other.swappable_pss = usage.swappable_pss;
  other.shared_dirty = usage.shared_dirty;
  other.private_clean = usage.private_clean;
  other.shared_clean = usage.shared_clean;
  SetCategory(env, info, dalvik_fields, dalvik);
  SetCategory(env, info, native_fields, native);
  SetCategory(env, info, other_fields, other);
  env->SetBooleanField(info, has_swap_pss, JNI_FALSE);

  jintArray stats = static_cast<jintArray>(env->GetObjectField(info, other_stats));
  if (stats != nullptr) {
    jsize length = env->GetArrayLength(stats);
    const jint zeros[32] = {};
    for (jsize offset = 0; offset < length;) {
      jsize count = length - offset;
      if (count > 32) count = 32;
      env->SetIntArrayRegion(stats, offset, count, zeros);
      if (env->ExceptionCheck()) break;
      offset += count;
    }
    env->DeleteLocalRef(stats);
  }
  return env->ExceptionCheck() ? JNI_FALSE : JNI_TRUE;
}

static void Debug_getMemoryInfo(JNIEnv* env, jclass clazz, jobject info) {
  Debug_getMemoryInfoPid(env, clazz, static_cast<jint>(GetCurrentProcessId()), info);
}

// No memtrack HAL: the graphics entries are 0, as AOSP's memtrack slots are
// when read_memtrack_memory finds nothing.
static void ZeroMemtrack(JNIEnv* env, jlongArray out) {
  if (out == nullptr) return;
  jsize length = env->GetArrayLength(out);
  if (length > 4) length = 4;
  if (length <= 0) return;
  jlong zeros[4] = {0, 0, 0, 0};
  env->SetLongArrayRegion(out, 0, length, zeros);
}

static jlong Debug_getPssPid(JNIEnv* env, jclass, jint pid, jlongArray outUssSwapPssRss,
                             jlongArray outMemtrack) {
  HANDLE process = OpenForMemory(pid);
  WorkingSetUsage usage;
  bool read = ReadWorkingSet(process, &usage);
  CloseProcess(process);
  if (!read) return 0;
  if (outUssSwapPssRss != nullptr) {
    jsize length = env->GetArrayLength(outUssSwapPssRss);
    if (length > 3) length = 3;
    jlong values[3] = {usage.uss, 0, usage.rss};
    if (length > 0) env->SetLongArrayRegion(outUssSwapPssRss, 0, length, values);
  }
  ZeroMemtrack(env, outMemtrack);
  return usage.pss;
}

static jlong Debug_getPss(JNIEnv* env, jclass clazz) {
  return Debug_getPssPid(env, clazz, static_cast<jint>(GetCurrentProcessId()), nullptr, nullptr);
}

static jlong Debug_getRssPid(JNIEnv* env, jclass, jint pid, jlongArray outMemtrack) {
  struct Counters {  // psapi.h's PROCESS_MEMORY_COUNTERS
    DWORD cb;
    DWORD page_fault_count;
    SIZE_T peak_working_set, working_set, quota_peak_paged, quota_paged,
        quota_peak_nonpaged, quota_nonpaged, pagefile, peak_pagefile;
  };
  static const GetProcessMemoryInfoFn info =
      Kernel32<GetProcessMemoryInfoFn>("K32GetProcessMemoryInfo");
  HANDLE process = OpenForMemory(pid);
  Counters counters = {};
  counters.cb = sizeof(counters);
  bool read = info != nullptr && process != nullptr && info(process, &counters, sizeof(counters));
  CloseProcess(process);
  if (!read) return 0;
  ZeroMemtrack(env, outMemtrack);
  return static_cast<jlong>(counters.working_set / 1024);
}

static jlong Debug_getRss(JNIEnv* env, jclass clazz) {
  return Debug_getRssPid(env, clazz, static_cast<jint>(GetCurrentProcessId()), nullptr);
}

// Debug.MEMINFO_* (AOSP's enum in android_os_Debug.cpp).
enum {
  kMemTotal, kMemFree, kMemBuffers, kMemCached, kMemShmem, kMemSlab, kMemSlabReclaimable,
  kMemSlabUnreclaimable, kMemSwapTotal, kMemSwapFree, kMemZramTotal, kMemMapped,
  kMemVmallocUsed, kMemPageTables, kMemKernelStack, kMemKernelReclaimable, kMemActive,
  kMemInactive, kMemUnevictable, kMemAvailable, kMemActiveAnon, kMemInactiveAnon,
  kMemActiveFile, kMemInactiveFile, kMemCmaTotal, kMemCmaFree, kMemSwapCached, kMemCount
};

// Only physical total and immediately available memory have direct documented
// counterparts here. PhysicalAvailable includes standby/free/zero lists, while
// SystemCache includes standby plus the system working set: subtracting them
// does not produce Linux MemFree. Paged kernel pools are not reclaimable slab,
// and commit-limit headroom is not actual pagefile usage. Those unsupported
// Linux breakdowns remain 0 rather than inventing values from unlike counters.
static void Debug_getMemInfo(JNIEnv* env, jclass, jlongArray out) {
  if (out == nullptr) {
    jniThrowNullPointerException(env, "out == null");
    return;
  }
  if (env->GetArrayLength(out) < kMemCount) {
    jniThrowRuntimeException(env, "outLen < MEMINFO_COUNT");
    return;
  }
  struct Performance {  // psapi.h's PERFORMANCE_INFORMATION
    DWORD cb;
    SIZE_T commit_total, commit_limit, commit_peak, physical_total, physical_available,
        system_cache, kernel_total, kernel_paged, kernel_nonpaged, page_size;
    DWORD handles, processes, threads;
  };
  static const GetPerformanceInfoFn performance =
      Kernel32<GetPerformanceInfoFn>("K32GetPerformanceInfo");
  Performance p = {};
  p.cb = sizeof(p);
  if (performance == nullptr || !performance(&p, sizeof(p))) {
    jniThrowRuntimeException(env, "SysMemInfo read failed");
    return;
  }
  auto kb = [&](SIZE_T pages) { return static_cast<jlong>(pages) * static_cast<jlong>(p.page_size) / 1024; };
  jlong values[kMemCount] = {};
  values[kMemTotal] = kb(p.physical_total);
  values[kMemAvailable] = kb(p.physical_available);
  env->SetLongArrayRegion(out, 0, kMemCount, values);
}

static const JNINativeMethod gDebugMethods[] = {
    {"getMemoryInfo", "(Landroid/os/Debug$MemoryInfo;)V", GESHER_JNI(Debug_getMemoryInfo)},
    {"getMemoryInfo", "(ILandroid/os/Debug$MemoryInfo;)Z", GESHER_JNI(Debug_getMemoryInfoPid)},
    {"getPss", "()J", GESHER_JNI(Debug_getPss)},
    {"getPss", "(I[J[J)J", GESHER_JNI(Debug_getPssPid)},
    {"getRss", "()J", GESHER_JNI(Debug_getRss)},
    {"getRss", "(I[J)J", GESHER_JNI(Debug_getRssPid)},
    {"getMemInfo", "([J)V", GESHER_JNI(Debug_getMemInfo)},
    {"getNativeHeapSize", "()J", GESHER_JNI(Debug_getNativeHeapSize)},
    {"getNativeHeapAllocatedSize", "()J", GESHER_JNI(Debug_getNativeHeapAllocatedSize)},
    {"getNativeHeapFreeSize", "()J", GESHER_JNI(Debug_getNativeHeapFreeSize)},
    {"getBinderLocalObjectCount", "()I", GESHER_JNI(android_os_Debug_getLocalObjectCount)},
    {"getBinderProxyObjectCount", "()I", GESHER_JNI(android_os_Debug_getProxyObjectCount)},
    {"getBinderDeathObjectCount", "()I", GESHER_JNI(android_os_Debug_getDeathObjectCount)},
    {"getBinderSentTransactions", "()I", GESHER_JNI(Debug_binderTransactionsUnavailable)},
    {"getBinderReceivedTransactions", "()I", GESHER_JNI(Debug_binderTransactionsUnavailable)},
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

// PerfettoTrackEventExtra and its nested arguments, fields, flows and tracks:
// the objects a trace event is built from. With tracing off they record
// nothing, but their classes are still initialized (each static initializer
// takes native_delete for its NativeAllocationRegistry): MessageQueue.quit
// builds a counter event, so HandlerThread.quit reached them. Natives with a
// String are @FastNative, the others @CriticalNative.

static jlong PerfettoExtra_init() {
  return reinterpret_cast<jlong>(calloc(1, 16));
}
static jlong PerfettoExtra_initS(JNIEnv*, jclass, jstring) {
  return PerfettoExtra_init();
}
static jlong PerfettoExtra_initSJ(JNIEnv*, jclass, jstring, jlong) {
  return PerfettoExtra_init();
}
static jlong PerfettoExtra_initJSJ(JNIEnv*, jclass, jlong, jstring, jlong) {
  return PerfettoExtra_init();
}
static jlong PerfettoExtra_delete() {
  return reinterpret_cast<jlong>(&PerfettoCategory_free);
}
static jlong PerfettoExtra_getExtraPtr(jlong ptr) {
  return ptr;
}
static void PerfettoExtra_j(jlong) {}
static void PerfettoExtra_jj(jlong, jlong) {}
static void PerfettoExtra_jjj(jlong, jlong, jlong) {}
static void PerfettoExtra_jd(jlong, jdouble) {}
static void PerfettoExtra_jjd(jlong, jlong, jdouble) {}
static void PerfettoExtra_jz(jlong, jboolean) {}
static void PerfettoExtra_jS(JNIEnv*, jclass, jlong, jstring) {}
static void PerfettoExtra_jjS(JNIEnv*, jclass, jlong, jlong, jstring) {}
static void PerfettoExtra_emit(JNIEnv*, jclass, jint, jlong, jstring, jlong) {}

#define PERFETTO_EXTRA_INIT {"native_init", "()J", GESHER_JNI(PerfettoExtra_init)}
#define PERFETTO_EXTRA_INIT_S \
  {"native_init", "(Ljava/lang/String;)J", GESHER_JNI(PerfettoExtra_initS)}
#define PERFETTO_EXTRA_DELETE {"native_delete", "()J", GESHER_JNI(PerfettoExtra_delete)}
#define PERFETTO_EXTRA_PTR {"native_get_extra_ptr", "(J)J", GESHER_JNI(PerfettoExtra_getExtraPtr)}

static const JNINativeMethod gPerfettoExtraMethods[] = {
    PERFETTO_EXTRA_INIT,
    PERFETTO_EXTRA_DELETE,
    {"native_add_arg", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
    {"native_clear_args", "(J)V", GESHER_JNI(PerfettoExtra_j)},
    {"native_emit", "(IJLjava/lang/String;J)V", GESHER_JNI(PerfettoExtra_emit)},
};
static const JNINativeMethod gPerfettoArgInt64Methods[] = {
    PERFETTO_EXTRA_INIT_S, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
};
static const JNINativeMethod gPerfettoArgBoolMethods[] = {
    PERFETTO_EXTRA_INIT_S, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JZ)V", GESHER_JNI(PerfettoExtra_jz)},
};
static const JNINativeMethod gPerfettoArgDoubleMethods[] = {
    PERFETTO_EXTRA_INIT_S, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JD)V", GESHER_JNI(PerfettoExtra_jd)},
};
static const JNINativeMethod gPerfettoArgStringMethods[] = {
    PERFETTO_EXTRA_INIT_S, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JLjava/lang/String;)V", GESHER_JNI(PerfettoExtra_jS)},
};
static const JNINativeMethod gPerfettoFieldInt64Methods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JJJ)V", GESHER_JNI(PerfettoExtra_jjj)},
};
static const JNINativeMethod gPerfettoFieldDoubleMethods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JJD)V", GESHER_JNI(PerfettoExtra_jjd)},
};
static const JNINativeMethod gPerfettoFieldStringMethods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JJLjava/lang/String;)V", GESHER_JNI(PerfettoExtra_jjS)},
};
static const JNINativeMethod gPerfettoFieldNestedMethods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_add_field", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
    {"native_set_id", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
};
static const JNINativeMethod gPerfettoProtoMethods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_add_field", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
    {"native_clear_fields", "(J)V", GESHER_JNI(PerfettoExtra_j)},
};
static const JNINativeMethod gPerfettoFlowMethods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_process_flow", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
    {"native_set_process_terminating_flow", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
};
static const JNINativeMethod gPerfettoNamedTrackMethods[] = {
    {"native_init", "(JLjava/lang/String;J)J", GESHER_JNI(PerfettoExtra_initJSJ)},
    PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
};
static const JNINativeMethod gPerfettoCounterTrackMethods[] = {
    {"native_init", "(Ljava/lang/String;J)J", GESHER_JNI(PerfettoExtra_initSJ)},
    PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
};
static const JNINativeMethod gPerfettoCounterInt64Methods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JJ)V", GESHER_JNI(PerfettoExtra_jj)},
};
static const JNINativeMethod gPerfettoCounterDoubleMethods[] = {
    PERFETTO_EXTRA_INIT, PERFETTO_EXTRA_DELETE, PERFETTO_EXTRA_PTR,
    {"native_set_value", "(JD)V", GESHER_JNI(PerfettoExtra_jd)},
};

#define PERFETTO_EXTRA_CLASS(suffix, methods) \
  RegisterMethodsOrDie(env, "android/os/PerfettoTrackEventExtra" suffix, methods, NELEM(methods))

static int register_android_os_PerfettoTrace(JNIEnv* env) {
  PERFETTO_EXTRA_CLASS("", gPerfettoExtraMethods);
  PERFETTO_EXTRA_CLASS("$ArgInt64", gPerfettoArgInt64Methods);
  PERFETTO_EXTRA_CLASS("$ArgBool", gPerfettoArgBoolMethods);
  PERFETTO_EXTRA_CLASS("$ArgDouble", gPerfettoArgDoubleMethods);
  PERFETTO_EXTRA_CLASS("$ArgString", gPerfettoArgStringMethods);
  PERFETTO_EXTRA_CLASS("$FieldInt64", gPerfettoFieldInt64Methods);
  PERFETTO_EXTRA_CLASS("$FieldDouble", gPerfettoFieldDoubleMethods);
  PERFETTO_EXTRA_CLASS("$FieldString", gPerfettoFieldStringMethods);
  PERFETTO_EXTRA_CLASS("$FieldNested", gPerfettoFieldNestedMethods);
  PERFETTO_EXTRA_CLASS("$Proto", gPerfettoProtoMethods);
  PERFETTO_EXTRA_CLASS("$Flow", gPerfettoFlowMethods);
  PERFETTO_EXTRA_CLASS("$NamedTrack", gPerfettoNamedTrackMethods);
  PERFETTO_EXTRA_CLASS("$CounterTrack", gPerfettoCounterTrackMethods);
  PERFETTO_EXTRA_CLASS("$CounterInt64", gPerfettoCounterInt64Methods);
  PERFETTO_EXTRA_CLASS("$CounterDouble", gPerfettoCounterDoubleMethods);
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

static int register_android_os_Debug(JNIEnv* env) {
  return RegisterMethodsOrDie(env, "android/os/Debug", gDebugMethods, NELEM(gDebugMethods));
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
    {"android_os_Debug", "android/os/Debug", register_android_os_Debug},
    {"gesher_shared_memory", "android/os/SharedMemory", ::register_gesher_shared_memory},
    {"android_text_AndroidCharacter", "android/text/AndroidCharacter",
     register_android_text_AndroidCharacter},
    {"com_android_internal_util_VirtualRefBasePtr", "com/android/internal/util/VirtualRefBasePtr",
     register_com_android_internal_util_VirtualRefBasePtr},
    {"android_util_jar_StrictJarFile", "android/util/jar/StrictJarFile",
     register_android_util_jar_StrictJarFile},
    {"android_os_MessageQueue", "android/os/MessageQueue", register_android_os_MessageQueue},
    {"gesher_MemoryFile", "android/os/MemoryFile", register_gesher_MemoryFile},
    {"gesher_MemoryIntArray", "android/util/MemoryIntArray", register_gesher_MemoryIntArray},
    {"gesher_Scrypt", "android/security/Scrypt", register_gesher_Scrypt},
    {"gesher_LocalSocketImpl", "android/net/LocalSocketImpl", register_gesher_LocalSocketImpl},
    {"gesher_CharsetUtils", "android/util/CharsetUtils", register_gesher_CharsetUtils},
    // FileObserver over gesher-base's inotify (docs/posix.md).
    {"android_os_FileObserver", "android/os/FileObserver", register_android_os_FileObserver},
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
    // What real apps need (cmake/aosp/framework_e2e.cmake, docs/e2e.md): ObjectAnimator's
    // property setters, android.database over SQLite.
    {"android_animation_PropertyValuesHolder", "android/animation/PropertyValuesHolder",
     register_android_animation_PropertyValuesHolder},
    {"android_database_CursorWindow", "android/database/CursorWindow",
     register_android_database_CursorWindow},
    {"android_database_SQLiteConnection", "android/database/sqlite/SQLiteConnection",
     register_android_database_SQLiteConnection},
    {"android_database_SQLiteGlobal", "android/database/sqlite/SQLiteGlobal",
     register_android_database_SQLiteGlobal},
    {"android_database_SQLiteDebug", "android/database/sqlite/SQLiteDebug",
     register_android_database_SQLiteDebug},
    {"android_database_SQLiteRawStatement", "android/database/sqlite/SQLiteRawStatement",
     register_android_database_SQLiteRawStatement},
    // Stand-ins for graphics and view natives on the attached app process's path
    // (attach_standins.cc); before the graphics rows, so real natives replace them.
    {"gesher_attach_standins", "android/graphics/Canvas", ::register_gesher_attach_standins},
    // hwui (AndroidRuntime.cpp registers it through libhwui's
    // register_android_graphics_classes), then the device-only stand-ins.
    {"gesher_graphics", "android/graphics/RenderNode", ::register_gesher_graphics},
    // The Java OpenGL ES/EGL bindings over ANGLE (compat/opengl/opengl_jni.cc).
    {"gesher_opengl", "com/google/android/gles_jni/EGLImpl", ::register_gesher_opengl},
    // The input natives, after the stand-ins they replace (each class is checked).
    {"gesher_input", "android/view/KeyEvent", ::register_gesher_input},
    {"gesher_native_activity", "android/app/NativeActivity", ::register_gesher_native_activity},
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
    {"gesher_Zygote", "com/gesher/server/Zygote", register_gesher_Zygote},
    {"gesher_Shell", "com/gesher/app/shell/Shell", register_gesher_Shell},
    {"gesher_media", "android/media/AudioSystem", register_gesher_media},
    // Surface parcels; after the stand-ins, whose Surface table it replaces in part.
    {"gesher_surface_parcel", "android/view/Surface", register_gesher_surface_parcel},
    // Location, sensors, cameras; after gesher_device_standins (whose sensor tables it replaces).
    {"gesher_devices", "android/hardware/SystemSensorManager", register_gesher_devices},
    {"gesher_WindowsHost", "com/gesher/server/WindowsHost", register_gesher_WindowsHost},
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
