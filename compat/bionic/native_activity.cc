// gesher: android.app.NativeActivity and android.view.InputQueue for an app's
// ELF libraries (docs/native-libs.md, "NativeActivity"; ADR 0032).
//
// After frameworks/base core/jni/android_app_NativeActivity.cpp,
// core/jni/android_view_InputQueue.cpp and native/android/input.cpp and
// native_activity.cpp (Apache 2.0, The Android Open Source Project), adapted
// to code the ELF loader loads, which is System V (ADR 0006):
//
//  - loadNativeCode dlopen()s the app's library (gesher-base hands ELF files
//    to the loader through the JNI bridge) and calls its
//    ANativeActivity_onCreate (or the manifest's android.app.func_name) with
//    an ANativeActivity whose vm and env are the bridge's System V proxies
//    (jni_bridge.h), so the .so's JNI calls work; clazz, the data paths,
//    sdkVersion, assetManager (the Java AssetManager's, as libandroid's
//    AAssetManager_fromJava gives) and obbPath are as on Android.
//  - The callbacks the .so fills in are System V functions: each is called
//    through a pointer of the System V type, with the thread set up for
//    loaded code (its TLS block, ADR 0014).
//  - The Activity's Surface (ViewRootImpl's, taken with Window.takeSurface:
//    a Win32Surface over the window's Win32 window, ADR 0029) is the
//    ANativeWindow of onNativeWindowCreated/Resized/RedrawNeeded/Destroyed.
//  - android.view.InputQueue's natives and libandroid's AInputQueue_*: the
//    events ViewRootImpl's NativePreIme/NativePostIme input stages send
//    (from the window's InputChannel, ADR 0027) wait in a queue whose fd the
//    .so's ALooper watches; finishing one goes back to the Java InputQueue on
//    the main thread. AInputEvent_*/AKeyEvent_*/AMotionEvent_* read
//    libinput's KeyEvent and MotionEvent, as on Android.
//  - ANativeActivity_finish/setWindowFormat/setWindowFlags/showSoftInput/
//    hideSoftInput: requests written to a pipe the main thread's Looper reads,
//    which calls the Activity's methods.
//
// register_gesher_native_activity (compat/framework/android_runtime.cc's
// table) registers the natives; the libandroid entries go into the bionic
// shim's table from a static initializer.

#define LOG_TAG "NativeActivity"

#include "jni_bridge.h"
#include "shim_internal.h"

#include <gesher/bionic_shim.h>

#include <android/input.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_runtime/android_util_AssetManager.h>
#include <input/Input.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <utils/Looper.h>
#include <utils/Mutex.h>

#include "android_os_MessageQueue.h"
#include "android_view_KeyEvent.h"
#include "android_view_MotionEvent.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// libandroid_runtime's (native_window_jni.cpp; compat/hwui/window), Microsoft ABI.
extern "C" ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);

namespace gesher::bionic {
namespace {

using android::InputEvent;
using android::InputEventType;
using android::KeyEvent;
using android::Looper;
using android::MotionEvent;
using android::sp;

// Calls a function pointer of loaded code, whose type is written with the
// default (Microsoft) ABI in the NDK's headers, as the System V function it is.
template <typename F>
struct SysvOf;
template <typename R, typename... A>
struct SysvOf<R (*)(A...)> {
  using type = R(BIONIC_SYSV*)(A...);
};
template <typename F, typename... Args>
auto CallSysv(F fn, Args... args) {
  EnterLoadedCode();
  return reinterpret_cast<typename SysvOf<F>::type>(fn)(args...);
}

JavaVM* g_vm = nullptr;

JNIEnv* MainEnv() {
  JNIEnv* env = nullptr;
  if (g_vm == nullptr || g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return nullptr;
  }
  return env;
}

// --- NativeActivity ---------------------------------------------------------------------

struct {
  jmethodID finish;
  jmethodID setWindowFlags;
  jmethodID setWindowFormat;
  jmethodID showIme;
  jmethodID hideIme;
} gNativeActivityClassInfo;

struct ActivityWork {
  int32_t cmd;
  int32_t arg1;
  int32_t arg2;
};

enum {
  CMD_FINISH = 1,
  CMD_SET_WINDOW_FORMAT,
  CMD_SET_WINDOW_FLAGS,
  CMD_SHOW_SOFT_INPUT,
  CMD_HIDE_SOFT_INPUT,
};

void WriteWork(int fd, int32_t cmd, int32_t arg1 = 0, int32_t arg2 = 0) {
  ActivityWork work{cmd, arg1, arg2};
  ssize_t res;
  do {
    res = write(fd, &work, sizeof(work));
  } while (res < 0 && errno == EINTR);
  if (res == static_cast<ssize_t>(sizeof(work))) return;
  if (res < 0) {
    ALOGW("Failed writing to work fd: %s", strerror(errno));
  } else {
    ALOGW("Truncated writing to work fd: %d", static_cast<int>(res));
  }
}

bool ReadWork(int fd, ActivityWork* out) {
  // Writes are whole records, and a pipe keeps them whole: read one record.
  size_t got = 0;
  while (got < sizeof(*out)) {
    ssize_t res = read(fd, reinterpret_cast<char*>(out) + got, sizeof(*out) - got);
    if (res < 0 && errno == EINTR) continue;
    if (res <= 0) {
      ALOGW("Failed reading work fd: %s", res < 0 ? strerror(errno) : "end of file");
      return false;
    }
    got += static_cast<size_t>(res);
  }
  return true;
}

// The native state of a NativeActivity: the ANativeActivity the .so sees.
struct NativeCode : public ANativeActivity {
  NativeCode(void* handle, void* create) {
    std::memset(static_cast<ANativeActivity*>(this), 0, sizeof(ANativeActivity));
    std::memset(&callbacks, 0, sizeof(callbacks));
    dlhandle = handle;
    createActivityFunc = create;
  }

  ~NativeCode() {
    if (callbacks.onDestroy != nullptr) CallSysv(callbacks.onDestroy, this);
    JNIEnv* real = MainEnv();
    if (real != nullptr) {
      if (clazz != nullptr) real->DeleteGlobalRef(clazz);
      if (javaAssetManager != nullptr) real->DeleteGlobalRef(javaAssetManager);
    }
    if (messageQueue != nullptr && mainWorkRead >= 0) {
      messageQueue->getLooper()->removeFd(mainWorkRead);
    }
    setWindow(nullptr);
    if (mainWorkRead >= 0) close(mainWorkRead);
    if (mainWorkWrite >= 0) close(mainWorkWrite);
    // As on Android, the library stays loaded.
  }

  // The Surface's ANativeWindow (a reference held here), or none.
  void setSurface(JNIEnv* real, jobject surface) {
    setWindow(surface != nullptr ? ANativeWindow_fromSurface(real, surface) : nullptr);
  }
  void setWindow(ANativeWindow* window) {  // takes `window`'s reference
    if (nativeWindow != nullptr) ANativeWindow_release(nativeWindow);
    nativeWindow = window;
  }

  ANativeActivityCallbacks callbacks;
  void* dlhandle;
  void* createActivityFunc;  // System V: (ANativeActivity*, void*, size_t)

  std::string internalDataPathObj;
  std::string externalDataPathObj;
  std::string obbPathObj;

  ANativeWindow* nativeWindow = nullptr;
  int32_t lastWindowWidth = 0;
  int32_t lastWindowHeight = 0;

  // The main thread's Looper reads requests from other threads here.
  int mainWorkRead = -1;
  int mainWorkWrite = -1;
  sp<android::MessageQueue> messageQueue;

  // Held in case the upper layers drop the AssetManager.
  jobject javaAssetManager = nullptr;
};

int MainWorkCallback(int /*fd*/, int events, void* data) {
  auto* code = static_cast<NativeCode*>(data);
  if ((events & Looper::EVENT_INPUT) == 0) return 1;
  ActivityWork work;
  if (!ReadWork(code->mainWorkRead, &work)) return 1;
  JNIEnv* env = MainEnv();
  if (env == nullptr) return 1;
  switch (work.cmd) {
    case CMD_FINISH:
      env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.finish);
      code->messageQueue->raiseAndClearException(env, "finish");
      break;
    case CMD_SET_WINDOW_FORMAT:
      env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.setWindowFormat, work.arg1);
      code->messageQueue->raiseAndClearException(env, "setWindowFormat");
      break;
    case CMD_SET_WINDOW_FLAGS:
      env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.setWindowFlags, work.arg1,
                          work.arg2);
      code->messageQueue->raiseAndClearException(env, "setWindowFlags");
      break;
    case CMD_SHOW_SOFT_INPUT:
      env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.showIme, work.arg1);
      code->messageQueue->raiseAndClearException(env, "showIme");
      break;
    case CMD_HIDE_SOFT_INPUT:
      env->CallVoidMethod(code->clazz, gNativeActivityClassInfo.hideIme, work.arg1);
      code->messageQueue->raiseAndClearException(env, "hideIme");
      break;
    default:
      ALOGW("Unknown work command: %d", work.cmd);
      break;
  }
  return 1;
}

thread_local std::string g_error_msg;

std::string Utf(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::string();
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out = chars != nullptr ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
  return out;
}

jlong JNICALL LoadNativeCode(JNIEnv* env, jobject clazz, jstring path, jstring funcName,
                             jobject messageQueue, jstring internalDataDir, jstring obbDir,
                             jstring externalDataDir, jint sdkVersion, jobject jAssetMgr,
                             jbyteArray savedState, jobject /*classLoader*/,
                             jstring /*libraryPath*/) {
  std::string pathStr = Utf(env, path);
  std::string funcStr = Utf(env, funcName);
  // The ELF file goes through gesher-base's dlopen to the loader (the JNI
  // bridge's hook), its DT_NEEDED libraries found next to it, as
  // System.loadLibrary's are; one global namespace (no linker namespaces).
  dlerror();
  void* handle = dlopen(pathStr.c_str(), RTLD_NOW);
  if (handle == nullptr) {
    const char* e = dlerror();
    g_error_msg = e != nullptr ? e : "dlopen failed";
    ALOGW("NativeActivity LoadNativeLibrary(\"%s\") failed: %s", pathStr.c_str(),
          g_error_msg.c_str());
    return 0;
  }
  void* create = dlsym(handle, funcStr.c_str());  // a raw System V address
  auto code = std::make_unique<NativeCode>(handle, create);
  if (code->createActivityFunc == nullptr) {
    const char* e = dlerror();
    g_error_msg = e != nullptr ? e : (funcStr + " not found");
    ALOGW("%s not found: %s", funcStr.c_str(), g_error_msg.c_str());
    return 0;
  }

  code->messageQueue = android::android_os_MessageQueue_getMessageQueue(env, messageQueue);
  if (code->messageQueue == nullptr) {
    g_error_msg = "Unable to retrieve native MessageQueue";
    ALOGW("%s", g_error_msg.c_str());
    return 0;
  }
  int msgpipe[2];
  if (pipe(msgpipe) != 0) {
    g_error_msg = std::string("could not create pipe: ") + strerror(errno);
    ALOGW("%s", g_error_msg.c_str());
    return 0;
  }
  code->mainWorkRead = msgpipe[0];
  code->mainWorkWrite = msgpipe[1];
  code->messageQueue->getLooper()->addFd(code->mainWorkRead, 0, Looper::EVENT_INPUT,
                                         MainWorkCallback, code.get());

  code->ANativeActivity::callbacks = &code->callbacks;
  // What the .so calls JNI through: the bridge's System V proxies.
  code->vm = ProxyJavaVm(env);
  code->env = ProxyJniEnv(env);
  code->clazz = env->NewGlobalRef(clazz);

  code->internalDataPathObj = Utf(env, internalDataDir);
  code->internalDataPath = code->internalDataPathObj.c_str();
  code->externalDataPathObj = Utf(env, externalDataDir);
  code->externalDataPath = code->externalDataPathObj.c_str();
  code->sdkVersion = sdkVersion;
  code->javaAssetManager = env->NewGlobalRef(jAssetMgr);
  code->assetManager = android::NdkAssetManagerForJavaObject(env, jAssetMgr);
  code->obbPathObj = Utf(env, obbDir);
  code->obbPath = code->obbPathObj.c_str();

  jbyte* rawSavedState = nullptr;
  jsize rawSavedSize = 0;
  if (savedState != nullptr) {
    rawSavedState = env->GetByteArrayElements(savedState, nullptr);
    rawSavedSize = env->GetArrayLength(savedState);
  }
  std::fprintf(stderr, "gesher NativeActivity: %s: calling %s\n", pathStr.c_str(), funcStr.c_str());
  std::fflush(stderr);
  using CreateFunc = void(BIONIC_SYSV*)(ANativeActivity*, void*, size_t);
  EnterLoadedCode();
  reinterpret_cast<CreateFunc>(code->createActivityFunc)(code.get(), rawSavedState,
                                                         static_cast<size_t>(rawSavedSize));
  if (rawSavedState != nullptr) env->ReleaseByteArrayElements(savedState, rawSavedState, 0);
  return reinterpret_cast<jlong>(code.release());
}

jstring JNICALL GetDlError(JNIEnv* env, jobject) {
  jstring result = env->NewStringUTF(g_error_msg.c_str());
  g_error_msg.clear();
  return result;
}

NativeCode* Code(jlong handle) { return reinterpret_cast<NativeCode*>(handle); }

void JNICALL UnloadNativeCode(JNIEnv*, jobject, jlong handle) {
  if (handle != 0) delete Code(handle);
}

#define GESHER_SIMPLE_CALLBACK(Name, member)                    \
  void JNICALL Name(JNIEnv*, jobject, jlong handle) {           \
    NativeCode* code = Code(handle);                            \
    if (code != nullptr && code->callbacks.member != nullptr) { \
      CallSysv(code->callbacks.member, code);                   \
    }                                                           \
  }
GESHER_SIMPLE_CALLBACK(OnStart, onStart)
GESHER_SIMPLE_CALLBACK(OnResume, onResume)
GESHER_SIMPLE_CALLBACK(OnPause, onPause)
GESHER_SIMPLE_CALLBACK(OnStop, onStop)
GESHER_SIMPLE_CALLBACK(OnConfigurationChanged, onConfigurationChanged)
GESHER_SIMPLE_CALLBACK(OnLowMemory, onLowMemory)
#undef GESHER_SIMPLE_CALLBACK

jbyteArray JNICALL OnSaveInstanceState(JNIEnv* env, jobject, jlong handle) {
  NativeCode* code = Code(handle);
  jbyteArray array = nullptr;
  if (code != nullptr && code->callbacks.onSaveInstanceState != nullptr) {
    size_t len = 0;
    auto* state = static_cast<jbyte*>(CallSysv(code->callbacks.onSaveInstanceState,
                                               static_cast<ANativeActivity*>(code), &len));
    if (len > 0 && state != nullptr) {
      array = env->NewByteArray(static_cast<jsize>(len));
      if (array != nullptr) env->SetByteArrayRegion(array, 0, static_cast<jsize>(len), state);
    }
    // The .so allocated it with its malloc: freed with its free (the shim's).
    if (state != nullptr) {
      using SysvFree = void(BIONIC_SYSV*)(void*);
      reinterpret_cast<SysvFree>(LookupBionicSymbol("free"))(state);
    }
  }
  return array;
}

void JNICALL OnWindowFocusChanged(JNIEnv*, jobject, jlong handle, jboolean focused) {
  NativeCode* code = Code(handle);
  if (code != nullptr && code->callbacks.onWindowFocusChanged != nullptr) {
    CallSysv(code->callbacks.onWindowFocusChanged, static_cast<ANativeActivity*>(code),
             focused ? 1 : 0);
  }
}

void JNICALL OnSurfaceCreated(JNIEnv* env, jobject, jlong handle, jobject surface) {
  NativeCode* code = Code(handle);
  if (code == nullptr) return;
  code->setSurface(env, surface);
  if (code->nativeWindow != nullptr) {
    code->lastWindowWidth = ANativeWindow_getWidth(code->nativeWindow);
    code->lastWindowHeight = ANativeWindow_getHeight(code->nativeWindow);
    std::fprintf(stderr, "gesher NativeActivity: window %dx%d\n", code->lastWindowWidth,
                 code->lastWindowHeight);
    if (code->callbacks.onNativeWindowCreated != nullptr) {
      CallSysv(code->callbacks.onNativeWindowCreated, static_cast<ANativeActivity*>(code),
               code->nativeWindow);
    }
  }
}

void JNICALL OnSurfaceChanged(JNIEnv* env, jobject, jlong handle, jobject surface, jint /*format*/,
                              jint /*width*/, jint /*height*/) {
  NativeCode* code = Code(handle);
  if (code == nullptr) return;
  ANativeWindow* old = code->nativeWindow;
  if (old != nullptr) ANativeWindow_acquire(old);  // kept while it is compared and reported
  code->setSurface(env, surface);
  if (old != code->nativeWindow) {
    if (old != nullptr && code->callbacks.onNativeWindowDestroyed != nullptr) {
      CallSysv(code->callbacks.onNativeWindowDestroyed, static_cast<ANativeActivity*>(code), old);
    }
    if (code->nativeWindow != nullptr) {
      if (code->callbacks.onNativeWindowCreated != nullptr) {
        CallSysv(code->callbacks.onNativeWindowCreated, static_cast<ANativeActivity*>(code),
                 code->nativeWindow);
      }
      code->lastWindowWidth = ANativeWindow_getWidth(code->nativeWindow);
      code->lastWindowHeight = ANativeWindow_getHeight(code->nativeWindow);
    }
  } else if (code->nativeWindow != nullptr) {
    // Maybe it resized?
    int32_t w = ANativeWindow_getWidth(code->nativeWindow);
    int32_t h = ANativeWindow_getHeight(code->nativeWindow);
    if (w != code->lastWindowWidth || h != code->lastWindowHeight) {
      code->lastWindowWidth = w;
      code->lastWindowHeight = h;
      if (code->callbacks.onNativeWindowResized != nullptr) {
        CallSysv(code->callbacks.onNativeWindowResized, static_cast<ANativeActivity*>(code),
                 code->nativeWindow);
      }
    }
  }
  if (old != nullptr) ANativeWindow_release(old);
}

void JNICALL OnSurfaceRedrawNeeded(JNIEnv*, jobject, jlong handle, jobject /*surface*/) {
  NativeCode* code = Code(handle);
  if (code != nullptr && code->nativeWindow != nullptr &&
      code->callbacks.onNativeWindowRedrawNeeded != nullptr) {
    CallSysv(code->callbacks.onNativeWindowRedrawNeeded, static_cast<ANativeActivity*>(code),
             code->nativeWindow);
  }
}

void JNICALL OnSurfaceDestroyed(JNIEnv*, jobject, jlong handle) {
  NativeCode* code = Code(handle);
  if (code == nullptr) return;
  if (code->nativeWindow != nullptr && code->callbacks.onNativeWindowDestroyed != nullptr) {
    CallSysv(code->callbacks.onNativeWindowDestroyed, static_cast<ANativeActivity*>(code),
             code->nativeWindow);
  }
  code->setWindow(nullptr);
}

void JNICALL OnInputQueueCreated(JNIEnv*, jobject, jlong handle, jlong queuePtr) {
  NativeCode* code = Code(handle);
  if (code != nullptr && code->callbacks.onInputQueueCreated != nullptr) {
    CallSysv(code->callbacks.onInputQueueCreated, static_cast<ANativeActivity*>(code),
             reinterpret_cast<AInputQueue*>(queuePtr));
  }
}

void JNICALL OnInputQueueDestroyed(JNIEnv*, jobject, jlong handle, jlong queuePtr) {
  NativeCode* code = Code(handle);
  if (code != nullptr && code->callbacks.onInputQueueDestroyed != nullptr) {
    CallSysv(code->callbacks.onInputQueueDestroyed, static_cast<ANativeActivity*>(code),
             reinterpret_cast<AInputQueue*>(queuePtr));
  }
}

void JNICALL OnContentRectChanged(JNIEnv*, jobject, jlong handle, jint x, jint y, jint w, jint h) {
  NativeCode* code = Code(handle);
  if (code != nullptr && code->callbacks.onContentRectChanged != nullptr) {
    ARect rect;
    rect.left = x;
    rect.top = y;
    rect.right = x + w;
    rect.bottom = y + h;
    CallSysv(code->callbacks.onContentRectChanged, static_cast<ANativeActivity*>(code),
             static_cast<const ARect*>(&rect));
  }
}

#define FN(f) reinterpret_cast<void*>(&f)
const JNINativeMethod kNativeActivityMethods[] = {
    {"loadNativeCode",
     "(Ljava/lang/String;Ljava/lang/String;Landroid/os/MessageQueue;Ljava/lang/String;"
     "Ljava/lang/String;Ljava/lang/String;ILandroid/content/res/AssetManager;[B"
     "Ljava/lang/ClassLoader;Ljava/lang/String;)J",
     FN(LoadNativeCode)},
    {"getDlError", "()Ljava/lang/String;", FN(GetDlError)},
    {"unloadNativeCode", "(J)V", FN(UnloadNativeCode)},
    {"onStartNative", "(J)V", FN(OnStart)},
    {"onResumeNative", "(J)V", FN(OnResume)},
    {"onSaveInstanceStateNative", "(J)[B", FN(OnSaveInstanceState)},
    {"onPauseNative", "(J)V", FN(OnPause)},
    {"onStopNative", "(J)V", FN(OnStop)},
    {"onConfigurationChangedNative", "(J)V", FN(OnConfigurationChanged)},
    {"onLowMemoryNative", "(J)V", FN(OnLowMemory)},
    {"onWindowFocusChangedNative", "(JZ)V", FN(OnWindowFocusChanged)},
    {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", FN(OnSurfaceCreated)},
    {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", FN(OnSurfaceChanged)},
    {"onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V", FN(OnSurfaceRedrawNeeded)},
    {"onSurfaceDestroyedNative", "(J)V", FN(OnSurfaceDestroyed)},
    {"onInputQueueCreatedNative", "(JJ)V", FN(OnInputQueueCreated)},
    {"onInputQueueDestroyedNative", "(JJ)V", FN(OnInputQueueDestroyed)},
    {"onContentRectChangedNative", "(JIIII)V", FN(OnContentRectChanged)},
};

// --- android.view.InputQueue -----------------------------------------------------------

struct {
  jmethodID finishInputEvent;
  jmethodID getNativePtr;
  jmethodID referenceGet;
} gInputQueueClassInfo;

using SysvLooperFunc = int(BIONIC_SYSV*)(int fd, int events, void* data);

class SysvLooperCallback : public android::LooperCallback {
 public:
  explicit SysvLooperCallback(SysvLooperFunc fn) : fn_(fn) {}
  int handleEvent(int fd, int events, void* data) override {
    EnterLoadedCode();
    return fn_(fd, events, data);
  }

 private:
  SysvLooperFunc fn_;
};

// The events ViewRootImpl gives the .so (InputQueue.sendInputEvent), between
// the Java InputQueue (on the main thread's Looper) and the .so's Looper,
// which watches the read end of a pipe that is readable while events wait.
class InputQueue : public android::MessageHandler {
 public:
  static sp<InputQueue> Create(JNIEnv* env, jobject weakQueue, const sp<Looper>& looper) {
    int fds[2];
    if (pipe(fds) != 0) {
      ALOGW("Could not create native input dispatching pipe: %s", strerror(errno));
      return nullptr;
    }
    return sp<InputQueue>::make(env, weakQueue, looper, fds[0], fds[1]);
  }

  InputQueue(JNIEnv* env, jobject weakQueue, const sp<Looper>& looper, int readFd, int writeFd)
      : read_fd_(readFd), write_fd_(writeFd), dispatch_looper_(looper) {
    weak_queue_ = env->NewGlobalRef(weakQueue);
  }

  ~InputQueue() override {
    if (handler_ != nullptr) dispatch_looper_->removeMessages(handler_);
    if (JNIEnv* env = MainEnv()) env->DeleteGlobalRef(weak_queue_);
    for (InputEvent* e : pending_) factory_.recycle(e);
    close(read_fd_);
    close(write_fd_);
  }

  void attachLooper(Looper* looper, int ident, SysvLooperFunc callback, void* data) {
    android::Mutex::Autolock _l(lock_);
    for (Looper* l : app_loopers_) {
      if (l == looper) return;
    }
    app_loopers_.push_back(looper);
    sp<android::LooperCallback> cb;
    if (callback != nullptr) cb = sp<SysvLooperCallback>::make(callback);
    looper->addFd(read_fd_, ident, Looper::EVENT_INPUT, cb, data);
  }

  void detachLooper() {
    android::Mutex::Autolock _l(lock_);
    for (Looper* l : app_loopers_) l->removeFd(read_fd_);
    app_loopers_.clear();
  }

  bool hasEvents() {
    android::Mutex::Autolock _l(lock_);
    return !pending_.empty();
  }

  android::status_t getEvent(InputEvent** outEvent) {
    android::Mutex::Autolock _l(lock_);
    *outEvent = nullptr;
    if (!pending_.empty()) {
      *outEvent = pending_.front();
      pending_.pop_front();
    }
    if (pending_.empty() && signaled_) {
      // One byte was written when the queue stopped being empty.
      char byte;
      ssize_t n;
      do {
        n = read(read_fd_, &byte, 1);
      } while (n < 0 && errno == EINTR);
      signaled_ = false;
    }
    return *outEvent != nullptr ? android::OK : android::WOULD_BLOCK;
  }

  bool preDispatchEvent(InputEvent* e) {
    if (e->getType() == InputEventType::KEY) {
      auto* keyEvent = static_cast<KeyEvent*>(e);
      if (keyEvent->getFlags() & AKEY_EVENT_FLAG_PREDISPATCH) {
        finishEvent(e, false);
        return true;
      }
    }
    return false;
  }

  void finishEvent(InputEvent* event, bool handled) {
    android::Mutex::Autolock _l(lock_);
    finished_.emplace_back(event, handled);
    if (finished_.size() == 1) dispatch_looper_->sendMessage(handler(), android::Message(1));
  }

  // On the main thread: the finished events back to the Java InputQueue.
  void handleMessage(const android::Message&) override {
    JNIEnv* env = MainEnv();
    if (env == nullptr) return;
    jobject queue = env->CallObjectMethod(weak_queue_, gInputQueueClassInfo.referenceGet);
    if (queue == nullptr) {
      ALOGW("InputQueue was finalized without being disposed");
      return;
    }
    while (true) {
      InputEvent* event;
      bool handled;
      {
        android::Mutex::Autolock _l(lock_);
        if (finished_.empty()) break;
        event = finished_.front().first;
        handled = finished_.front().second;
        finished_.erase(finished_.begin());
      }
      env->CallVoidMethod(queue, gInputQueueClassInfo.finishInputEvent,
                          reinterpret_cast<jlong>(event), static_cast<jboolean>(handled));
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      android::Mutex::Autolock _l(lock_);
      factory_.recycle(event);
    }
    env->DeleteLocalRef(queue);
  }

  KeyEvent* createKeyEvent() {
    android::Mutex::Autolock _l(lock_);
    return factory_.createKeyEvent();
  }
  MotionEvent* createMotionEvent() {
    android::Mutex::Autolock _l(lock_);
    return factory_.createMotionEvent();
  }

  void enqueueEvent(InputEvent* event) {
    android::Mutex::Autolock _l(lock_);
    pending_.push_back(event);
    if (!signaled_) {
      char payload = '\0';
      ssize_t res;
      do {
        res = write(write_fd_, &payload, 1);
      } while (res < 0 && errno == EINTR);
      if (res < 0) {
        ALOGW("Failed writing to dispatch fd: %s", strerror(errno));
      } else {
        signaled_ = true;
      }
    }
  }

 private:
  sp<android::MessageHandler> handler() {
    if (handler_ == nullptr) {
      // The Java InputQueue holds a strong reference while events are finished.
      sp<android::MessageHandler> self = sp<android::MessageHandler>::fromExisting(this);
      handler_ = sp<android::WeakMessageHandler>::make(android::wp<android::MessageHandler>(self));
    }
    return handler_;
  }

  const int read_fd_;
  const int write_fd_;
  sp<Looper> dispatch_looper_;
  sp<android::WeakMessageHandler> handler_;
  jobject weak_queue_ = nullptr;
  android::Mutex lock_;
  std::vector<Looper*> app_loopers_;
  std::deque<InputEvent*> pending_;
  bool signaled_ = false;
  std::vector<std::pair<InputEvent*, bool>> finished_;
  android::PooledInputEventFactory factory_;
};

InputQueue* Queue(AInputQueue* q) { return reinterpret_cast<InputQueue*>(q); }

jlong JNICALL InputQueueInit(JNIEnv* env, jclass, jobject queueWeak, jobject jMsgQueue) {
  sp<android::MessageQueue> messageQueue =
      android::android_os_MessageQueue_getMessageQueue(env, jMsgQueue);
  if (messageQueue == nullptr) {
    jniThrowRuntimeException(env, "MessageQueue is not initialized.");
    return 0;
  }
  sp<InputQueue> queue = InputQueue::Create(env, queueWeak, messageQueue->getLooper());
  if (queue == nullptr) {
    jniThrowRuntimeException(env, "InputQueue failed to initialize");
    return 0;
  }
  queue->incStrong(&gInputQueueClassInfo);
  return reinterpret_cast<jlong>(queue.get());
}

void JNICALL InputQueueDispose(JNIEnv*, jclass, jlong ptr) {
  sp<InputQueue> queue = sp<InputQueue>::fromExisting(reinterpret_cast<InputQueue*>(ptr));
  queue->detachLooper();
  queue->decStrong(&gInputQueueClassInfo);
}

jlong JNICALL InputQueueSendKeyEvent(JNIEnv* env, jclass, jlong ptr, jobject eventObj,
                                     jboolean predispatch) {
  auto* queue = reinterpret_cast<InputQueue*>(ptr);
  KeyEvent* event = queue->createKeyEvent();
  *event = android::android_view_KeyEvent_obtainAsCopy(env, eventObj);
  if (predispatch) event->setFlags(event->getFlags() | AKEY_EVENT_FLAG_PREDISPATCH);
  queue->enqueueEvent(event);
  return reinterpret_cast<jlong>(event);
}

jlong JNICALL InputQueueSendMotionEvent(JNIEnv* env, jclass, jlong ptr, jobject eventObj) {
  auto* queue = reinterpret_cast<InputQueue*>(ptr);
  MotionEvent* original = android::android_view_MotionEvent_getNativePtr(env, eventObj);
  if (original == nullptr) {
    jniThrowRuntimeException(env, "Could not obtain MotionEvent pointer.");
    return -1;
  }
  MotionEvent* event = queue->createMotionEvent();
  event->copyFrom(original, /*keepHistory=*/true);
  queue->enqueueEvent(event);
  return reinterpret_cast<jlong>(event);
}

const JNINativeMethod kInputQueueMethods[] = {
    {"nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;)J",
     FN(InputQueueInit)},
    {"nativeDispose", "(J)V", FN(InputQueueDispose)},
    {"nativeSendKeyEvent", "(JLandroid/view/KeyEvent;Z)J", FN(InputQueueSendKeyEvent)},
    {"nativeSendMotionEvent", "(JLandroid/view/MotionEvent;)J", FN(InputQueueSendMotionEvent)},
};
#undef FN

// --- libandroid: ANativeActivity_*, AInputQueue_*, AInputEvent_*, AKeyEvent_*, AMotionEvent_*

NativeCode* Activity(ANativeActivity* a) { return static_cast<NativeCode*>(a); }

void BIONIC_SYSV ActivityFinish(ANativeActivity* a) {
  WriteWork(Activity(a)->mainWorkWrite, CMD_FINISH);
}
void BIONIC_SYSV ActivitySetWindowFormat(ANativeActivity* a, int32_t format) {
  WriteWork(Activity(a)->mainWorkWrite, CMD_SET_WINDOW_FORMAT, format);
}
void BIONIC_SYSV ActivitySetWindowFlags(ANativeActivity* a, uint32_t values, uint32_t mask) {
  WriteWork(Activity(a)->mainWorkWrite, CMD_SET_WINDOW_FLAGS, static_cast<int32_t>(values),
            static_cast<int32_t>(mask));
}
void BIONIC_SYSV ActivityShowSoftInput(ANativeActivity* a, uint32_t flags) {
  WriteWork(Activity(a)->mainWorkWrite, CMD_SHOW_SOFT_INPUT, static_cast<int32_t>(flags));
}
void BIONIC_SYSV ActivityHideSoftInput(ANativeActivity* a, uint32_t flags) {
  WriteWork(Activity(a)->mainWorkWrite, CMD_HIDE_SOFT_INPUT, static_cast<int32_t>(flags));
}

const InputEvent* Ev(const AInputEvent* e) { return static_cast<const InputEvent*>(e); }
const KeyEvent* Key(const AInputEvent* e) { return static_cast<const KeyEvent*>(e); }
const MotionEvent* Motion(const AInputEvent* e) { return static_cast<const MotionEvent*>(e); }

int32_t BIONIC_SYSV EventGetType(const AInputEvent* e) {
  return static_cast<int32_t>(Ev(e)->getType());
}
int32_t BIONIC_SYSV EventGetDeviceId(const AInputEvent* e) { return Ev(e)->getDeviceId(); }
int32_t BIONIC_SYSV EventGetSource(const AInputEvent* e) {
  return static_cast<int32_t>(Ev(e)->getSource());
}
void BIONIC_SYSV EventRelease(const AInputEvent* e) { delete e; }

int32_t BIONIC_SYSV KeyGetAction(const AInputEvent* e) { return Key(e)->getAction(); }
int32_t BIONIC_SYSV KeyGetFlags(const AInputEvent* e) { return Key(e)->getFlags(); }
int32_t BIONIC_SYSV KeyGetKeyCode(const AInputEvent* e) { return Key(e)->getKeyCode(); }
int32_t BIONIC_SYSV KeyGetScanCode(const AInputEvent* e) { return Key(e)->getScanCode(); }
int32_t BIONIC_SYSV KeyGetMetaState(const AInputEvent* e) { return Key(e)->getMetaState(); }
int32_t BIONIC_SYSV KeyGetRepeatCount(const AInputEvent* e) { return Key(e)->getRepeatCount(); }
int64_t BIONIC_SYSV KeyGetDownTime(const AInputEvent* e) { return Key(e)->getDownTime(); }
int64_t BIONIC_SYSV KeyGetEventTime(const AInputEvent* e) { return Key(e)->getEventTime(); }
const AInputEvent* BIONIC_SYSV KeyFromJava(JNIEnv* env, jobject keyEvent) {
  auto event = std::make_unique<KeyEvent>();
  *event = android::android_view_KeyEvent_obtainAsCopy(RealJniEnv(env), keyEvent);
  return event.release();
}

int32_t BIONIC_SYSV MotionGetAction(const AInputEvent* e) { return Motion(e)->getAction(); }
int32_t BIONIC_SYSV MotionGetFlags(const AInputEvent* e) {
  return static_cast<int32_t>(Motion(e)->getFlags().get() & ~AMOTION_EVENT_PRIVATE_FLAG_MASK);
}
int32_t BIONIC_SYSV MotionGetMetaState(const AInputEvent* e) { return Motion(e)->getMetaState(); }
int32_t BIONIC_SYSV MotionGetButtonState(const AInputEvent* e) {
  return Motion(e)->getButtonState();
}
int32_t BIONIC_SYSV MotionGetEdgeFlags(const AInputEvent* e) { return Motion(e)->getEdgeFlags(); }
int64_t BIONIC_SYSV MotionGetDownTime(const AInputEvent* e) { return Motion(e)->getDownTime(); }
int64_t BIONIC_SYSV MotionGetEventTime(const AInputEvent* e) { return Motion(e)->getEventTime(); }
float BIONIC_SYSV MotionGetXOffset(const AInputEvent* e) { return Motion(e)->getRawXOffset(); }
float BIONIC_SYSV MotionGetYOffset(const AInputEvent* e) { return Motion(e)->getRawYOffset(); }
float BIONIC_SYSV MotionGetXPrecision(const AInputEvent* e) { return Motion(e)->getXPrecision(); }
float BIONIC_SYSV MotionGetYPrecision(const AInputEvent* e) { return Motion(e)->getYPrecision(); }
size_t BIONIC_SYSV MotionGetPointerCount(const AInputEvent* e) {
  return Motion(e)->getPointerCount();
}
int32_t BIONIC_SYSV MotionGetPointerId(const AInputEvent* e, size_t i) {
  return Motion(e)->getPointerId(i);
}
int32_t BIONIC_SYSV MotionGetToolType(const AInputEvent* e, size_t i) {
  return static_cast<int32_t>(Motion(e)->getToolType(i));
}
#define GESHER_POINTER_FLOAT(Suffix)                                                          \
  float BIONIC_SYSV MotionGet##Suffix(const AInputEvent* e, size_t i) {                       \
    return Motion(e)->get##Suffix(i);                                                         \
  }                                                                                           \
  float BIONIC_SYSV MotionGet##Suffix##Historical(const AInputEvent* e, size_t i, size_t h) { \
    return Motion(e)->getHistorical##Suffix(i, h);                                            \
  }
GESHER_POINTER_FLOAT(RawX)
GESHER_POINTER_FLOAT(RawY)
GESHER_POINTER_FLOAT(X)
GESHER_POINTER_FLOAT(Y)
GESHER_POINTER_FLOAT(Pressure)
GESHER_POINTER_FLOAT(Size)
GESHER_POINTER_FLOAT(TouchMajor)
GESHER_POINTER_FLOAT(TouchMinor)
GESHER_POINTER_FLOAT(ToolMajor)
GESHER_POINTER_FLOAT(ToolMinor)
GESHER_POINTER_FLOAT(Orientation)
#undef GESHER_POINTER_FLOAT
float BIONIC_SYSV MotionGetAxisValue(const AInputEvent* e, int32_t axis, size_t i) {
  return Motion(e)->getAxisValue(axis, i);
}
size_t BIONIC_SYSV MotionGetHistorySize(const AInputEvent* e) {
  return Motion(e)->getHistorySize();
}
int64_t BIONIC_SYSV MotionGetHistoricalEventTime(const AInputEvent* e, size_t h) {
  return Motion(e)->getHistoricalEventTime(h);
}
float BIONIC_SYSV MotionGetHistoricalAxisValue(const AInputEvent* e, int32_t axis, size_t i,
                                               size_t h) {
  return Motion(e)->getHistoricalAxisValue(axis, i, h);
}
int32_t BIONIC_SYSV MotionGetActionButton(const AInputEvent* e) {
  return Motion(e)->getActionButton();
}
int32_t BIONIC_SYSV MotionGetClassification(const AInputEvent* e) {
  switch (Motion(e)->getClassification()) {
    case android::MotionClassification::NONE:
      return AMOTION_EVENT_CLASSIFICATION_NONE;
    case android::MotionClassification::AMBIGUOUS_GESTURE:
      return AMOTION_EVENT_CLASSIFICATION_AMBIGUOUS_GESTURE;
    case android::MotionClassification::DEEP_PRESS:
      return AMOTION_EVENT_CLASSIFICATION_DEEP_PRESS;
    case android::MotionClassification::TWO_FINGER_SWIPE:
      return AMOTION_EVENT_CLASSIFICATION_TWO_FINGER_SWIPE;
    case android::MotionClassification::MULTI_FINGER_SWIPE:
      return AMOTION_EVENT_CLASSIFICATION_MULTI_FINGER_SWIPE;
    case android::MotionClassification::PINCH:
      return AMOTION_EVENT_CLASSIFICATION_PINCH;
  }
  return AMOTION_EVENT_CLASSIFICATION_NONE;
}
const AInputEvent* BIONIC_SYSV MotionFromJava(JNIEnv* env, jobject motionEvent) {
  MotionEvent* src = android::android_view_MotionEvent_getNativePtr(RealJniEnv(env), motionEvent);
  if (src == nullptr) return nullptr;
  auto* event = new MotionEvent();
  event->copyFrom(src, true);
  return event;
}
jobject BIONIC_SYSV EventToJava(JNIEnv* env, const AInputEvent* e) {
  JNIEnv* real = RealJniEnv(env);
  switch (Ev(e)->getType()) {
    case InputEventType::MOTION:
      return android::android_view_MotionEvent_obtainAsCopy(real, *Motion(e)).release();
    case InputEventType::KEY:
      return android::android_view_KeyEvent_obtainAsCopy(real, *Key(e)).release();
    default:
      LOG_ALWAYS_FATAL("Unexpected event type %d in AInputEvent_toJava.",
                       static_cast<int>(Ev(e)->getType()));
  }
  return nullptr;
}

void BIONIC_SYSV QueueAttachLooper(AInputQueue* q, void* looper, int ident,
                                   SysvLooperFunc callback, void* data) {
  Queue(q)->attachLooper(static_cast<Looper*>(looper), ident, callback, data);
}
void BIONIC_SYSV QueueDetachLooper(AInputQueue* q) { Queue(q)->detachLooper(); }
int32_t BIONIC_SYSV QueueHasEvents(AInputQueue* q) { return Queue(q)->hasEvents() ? 1 : 0; }
int32_t BIONIC_SYSV QueueGetEvent(AInputQueue* q, AInputEvent** outEvent) {
  InputEvent* event = nullptr;
  int32_t res = Queue(q)->getEvent(&event);
  *outEvent = event;
  return res;
}
int32_t BIONIC_SYSV QueuePreDispatchEvent(AInputQueue* q, AInputEvent* e) {
  return Queue(q)->preDispatchEvent(static_cast<InputEvent*>(e)) ? 1 : 0;
}
void BIONIC_SYSV QueueFinishEvent(AInputQueue* q, AInputEvent* e, int handled) {
  Queue(q)->finishEvent(static_cast<InputEvent*>(e), handled != 0);
}
AInputQueue* BIONIC_SYSV QueueFromJava(JNIEnv* env, jobject inputQueue) {
  JNIEnv* real = RealJniEnv(env);
  jlong ptr = real->CallLongMethod(inputQueue, gInputQueueClassInfo.getNativePtr);
  return reinterpret_cast<AInputQueue*>(ptr);
}

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
#define PF(Suffix)                                  \
  E("AMotionEvent_get" #Suffix, MotionGet##Suffix), \
      E("AMotionEvent_getHistorical" #Suffix, MotionGet##Suffix##Historical)
  static const Entry kEntries[] = {
      E("ANativeActivity_finish", ActivityFinish),
      E("ANativeActivity_setWindowFormat", ActivitySetWindowFormat),
      E("ANativeActivity_setWindowFlags", ActivitySetWindowFlags),
      E("ANativeActivity_showSoftInput", ActivityShowSoftInput),
      E("ANativeActivity_hideSoftInput", ActivityHideSoftInput),
      E("AInputEvent_getType", EventGetType),
      E("AInputEvent_getDeviceId", EventGetDeviceId),
      E("AInputEvent_getSource", EventGetSource),
      E("AInputEvent_release", EventRelease),
      E("AInputEvent_toJava", EventToJava),
      E("AKeyEvent_getAction", KeyGetAction),
      E("AKeyEvent_getFlags", KeyGetFlags),
      E("AKeyEvent_getKeyCode", KeyGetKeyCode),
      E("AKeyEvent_getScanCode", KeyGetScanCode),
      E("AKeyEvent_getMetaState", KeyGetMetaState),
      E("AKeyEvent_getRepeatCount", KeyGetRepeatCount),
      E("AKeyEvent_getDownTime", KeyGetDownTime),
      E("AKeyEvent_getEventTime", KeyGetEventTime),
      E("AKeyEvent_fromJava", KeyFromJava),
      E("AMotionEvent_getAction", MotionGetAction),
      E("AMotionEvent_getFlags", MotionGetFlags),
      E("AMotionEvent_getMetaState", MotionGetMetaState),
      E("AMotionEvent_getButtonState", MotionGetButtonState),
      E("AMotionEvent_getEdgeFlags", MotionGetEdgeFlags),
      E("AMotionEvent_getDownTime", MotionGetDownTime),
      E("AMotionEvent_getEventTime", MotionGetEventTime),
      E("AMotionEvent_getXOffset", MotionGetXOffset),
      E("AMotionEvent_getYOffset", MotionGetYOffset),
      E("AMotionEvent_getXPrecision", MotionGetXPrecision),
      E("AMotionEvent_getYPrecision", MotionGetYPrecision),
      E("AMotionEvent_getPointerCount", MotionGetPointerCount),
      E("AMotionEvent_getPointerId", MotionGetPointerId),
      E("AMotionEvent_getToolType", MotionGetToolType),
      PF(RawX), PF(RawY), PF(X), PF(Y), PF(Pressure), PF(Size), PF(TouchMajor), PF(TouchMinor),
      PF(ToolMajor), PF(ToolMinor), PF(Orientation),
      E("AMotionEvent_getAxisValue", MotionGetAxisValue),
      E("AMotionEvent_getHistorySize", MotionGetHistorySize),
      E("AMotionEvent_getHistoricalEventTime", MotionGetHistoricalEventTime),
      E("AMotionEvent_getHistoricalAxisValue", MotionGetHistoricalAxisValue),
      E("AMotionEvent_getActionButton", MotionGetActionButton),
      E("AMotionEvent_getClassification", MotionGetClassification),
      E("AMotionEvent_fromJava", MotionFromJava),
      E("AInputQueue_attachLooper", QueueAttachLooper),
      E("AInputQueue_detachLooper", QueueDetachLooper),
      E("AInputQueue_hasEvents", QueueHasEvents),
      E("AInputQueue_getEvent", QueueGetEvent),
      E("AInputQueue_preDispatchEvent", QueuePreDispatchEvent),
      E("AInputQueue_finishEvent", QueueFinishEvent),
      E("AInputQueue_fromJava", QueueFromJava),
  };
#undef PF
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

int RegisterNativeActivity(JNIEnv* env) {
  jclass clazz = env->FindClass("android/app/NativeActivity");
  if (clazz == nullptr) return -1;
  gNativeActivityClassInfo.finish = env->GetMethodID(clazz, "finish", "()V");
  gNativeActivityClassInfo.setWindowFlags = env->GetMethodID(clazz, "setWindowFlags", "(II)V");
  gNativeActivityClassInfo.setWindowFormat = env->GetMethodID(clazz, "setWindowFormat", "(I)V");
  gNativeActivityClassInfo.showIme = env->GetMethodID(clazz, "showIme", "(I)V");
  gNativeActivityClassInfo.hideIme = env->GetMethodID(clazz, "hideIme", "(I)V");
  env->DeleteLocalRef(clazz);
  if (gNativeActivityClassInfo.finish == nullptr || gNativeActivityClassInfo.hideIme == nullptr) {
    return -1;
  }
  return jniRegisterNativeMethods(env, "android/app/NativeActivity", kNativeActivityMethods,
                                  sizeof(kNativeActivityMethods) / sizeof(kNativeActivityMethods[0]));
}

int RegisterInputQueue(JNIEnv* env) {
  jclass clazz = env->FindClass("android/view/InputQueue");
  if (clazz == nullptr) return -1;
  gInputQueueClassInfo.finishInputEvent = env->GetMethodID(clazz, "finishInputEvent", "(JZ)V");
  gInputQueueClassInfo.getNativePtr = env->GetMethodID(clazz, "getNativePtr", "()J");
  env->DeleteLocalRef(clazz);
  jclass ref = env->FindClass("java/lang/ref/Reference");
  if (ref == nullptr) return -1;
  gInputQueueClassInfo.referenceGet = env->GetMethodID(ref, "get", "()Ljava/lang/Object;");
  env->DeleteLocalRef(ref);
  if (gInputQueueClassInfo.finishInputEvent == nullptr ||
      gInputQueueClassInfo.getNativePtr == nullptr || gInputQueueClassInfo.referenceGet == nullptr) {
    return -1;
  }
  return jniRegisterNativeMethods(env, "android/view/InputQueue", kInputQueueMethods,
                                  sizeof(kInputQueueMethods) / sizeof(kInputQueueMethods[0]));
}

}  // namespace
}  // namespace gesher::bionic

// NativeActivity's and InputQueue's natives (each skipped when its class is
// not on the class path), from compat/framework/android_runtime.cc's table.
int register_gesher_native_activity(JNIEnv* env) {
  env->GetJavaVM(&gesher::bionic::g_vm);
  const struct {
    const char* className;
    int (*registerFunction)(JNIEnv*);
  } kRows[] = {
      {"android/app/NativeActivity", gesher::bionic::RegisterNativeActivity},
      {"android/view/InputQueue", gesher::bionic::RegisterInputQueue},
  };
  for (const auto& r : kRows) {
    jclass clazz = env->FindClass(r.className);
    if (clazz == nullptr) {
      env->ExceptionClear();
      continue;
    }
    env->DeleteLocalRef(clazz);
    if (r.registerFunction(env) < 0) {
      ALOGE("libandroid_runtime: registering the natives of %s failed", r.className);
      return -1;
    }
  }
  return 0;
}
