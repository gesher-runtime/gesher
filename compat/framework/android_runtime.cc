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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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

static const JNINativeMethod gDebugMethods[] = {
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
