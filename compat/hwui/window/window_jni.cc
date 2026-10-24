// Gesher: com.gesher.app.window.NativeWindow's natives (gesher-app,
// app/java/com/gesher/app/window/NativeWindow.java): a Win32 window and the
// android.view.Surface that draws into it (Win32Surface). Registered from
// compat/hwui/graphics_runtime.cc when the class is on the class path.
//
// The functions are JNICALL, which is System V under GESHER, as ART calls
// natives (docs/decisions/0006-managed-abi.md).

#include <jni.h>
#include <string.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <android/native_window_jni.h>
#include <android_runtime/android_view_Surface.h>
#include <nativehelper/JNIHelp.h>
#include <system/window.h>

#include "gesher_device_standins.h"
#include "gesher_window.h"
#include "win32_surface.h"

using gesher::Win32Surface;

namespace {

Win32Surface* Get(jlong ptr) {
  return reinterpret_cast<Win32Surface*>(ptr);
}

// The windows whose NativeWindow is open (each holds a strong reference
// until nDestroy): what a stand-in SurfaceControl's tag may name.
std::mutex gOpenMutex;
std::set<Win32Surface*> gOpen;

std::string Utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::string();
  const char* utf = env->GetStringUTFChars(s, nullptr);
  std::string out = utf != nullptr ? utf : "";
  if (utf != nullptr) env->ReleaseStringUTFChars(s, utf);
  return out;
}

// BLASTBufferQueue.nativeGetSurface's provider (gesher_device_standins.h):
// the Surface over the window whose SurfaceControl the adapter was updated
// with (NativeWindow.tagSurfaceControl), or null.
jobject BlastSurface(JNIEnv* env, int64_t blast) {
  auto* tagged = reinterpret_cast<Win32Surface*>(static_cast<intptr_t>(gesher_standin_tag(blast)));
  if (tagged == nullptr) return nullptr;
  android::sp<android::Surface> surface;
  {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    if (gOpen.count(tagged) == 0) return nullptr;
    surface = android::sp<android::Surface>::fromExisting(tagged);
  }
  return android::android_view_Surface_createFromSurface(env, surface);
}

jlong JNICALL NativeWindow_create(JNIEnv* env, jclass, jstring title, jint width, jint height) {
  gesher::Window* window = gesher::Window::Create(Utf8(env, title), width, height);
  if (window == nullptr) return 0;
  auto* surface = new Win32Surface(window);
  surface->incStrong(reinterpret_cast<void*>(&NativeWindow_create));
  {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    gOpen.insert(surface);
  }
  return reinterpret_cast<jlong>(surface);
}

void JNICALL NativeWindow_destroy(JNIEnv*, jclass, jlong ptr) {
  {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    gOpen.erase(Get(ptr));
  }
  Get(ptr)->decStrong(reinterpret_cast<void*>(&NativeWindow_create));
}

void JNICALL NativeWindow_hide(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->window()->Hide();
}

void JNICALL NativeWindow_setTitle(JNIEnv* env, jclass, jlong ptr, jstring title) {
  Get(ptr)->window()->SetTitle(Utf8(env, title));
}

void JNICALL NativeWindow_setResizable(JNIEnv*, jclass, jlong ptr, jboolean resizable) {
  Get(ptr)->window()->SetResizable(resizable);
}

void JNICALL NativeWindow_setClientSize(JNIEnv*, jclass, jlong ptr, jint width, jint height) {
  Get(ptr)->window()->SetClientSize(width, height);
}

void JNICALL NativeWindow_setMinClientSize(JNIEnv*, jclass, jlong ptr, jint width,
                                           jint height) {
  Get(ptr)->window()->SetMinClientSize(width, height);
}

// The window's icon from an .ico file (title bar, taskbar); false when it
// cannot be read.
jboolean JNICALL NativeWindow_setIcon(JNIEnv* env, jclass, jlong ptr, jstring path) {
  return Get(ptr)->window()->SetIcon(Utf8(env, path)) ? JNI_TRUE : JNI_FALSE;
}

// The System.AppUserModel.ID the window got (the process's), or null.
jstring JNICALL NativeWindow_getAppUserModelId(JNIEnv* env, jclass, jlong ptr) {
  const std::string& id = Get(ptr)->window()->app_user_model_id();
  return id.empty() ? nullptr : env->NewStringUTF(id.c_str());
}

// Tags a stand-in SurfaceControl handle with the window, so the BLAST adapter
// ViewRootImpl makes for it hands out this window's Surface.
void JNICALL NativeWindow_tagSurfaceControl(JNIEnv*, jclass, jlong ptr, jlong surface_control) {
  gesher_standin_set_tag(surface_control, ptr);
}

jint JNICALL NativeWindow_getEventFd(JNIEnv*, jclass, jlong ptr) {
  return Get(ptr)->window()->event_fd();
}

// The next queued WindowEvent into out[0..12] (type, x, y, button, key, scan,
// repeat, modifiers, delta, ch, width, height, time_ms); returns its type, or
// 0 when none is queued.
jint JNICALL NativeWindow_pollEvent(JNIEnv* env, jclass, jlong ptr, jdoubleArray out) {
  gesher::WindowEvent e;
  if (!Get(ptr)->window()->PollEvent(&e)) return 0;
  const jdouble v[13] = {
      static_cast<jdouble>(e.type),   e.x, e.y, static_cast<jdouble>(e.button),
      static_cast<jdouble>(e.key),    static_cast<jdouble>(e.scan),
      static_cast<jdouble>(e.repeat), static_cast<jdouble>(e.modifiers), e.delta,
      static_cast<jdouble>(e.ch),     static_cast<jdouble>(e.width),
      static_cast<jdouble>(e.height), static_cast<jdouble>(e.time_ms)};
  env->SetDoubleArrayRegion(out, 0, 13, v);
  return e.type;
}

void JNICALL NativeWindow_showOffscreen(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->window()->ShowOffscreen();
}

void JNICALL NativeWindow_show(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->window()->Show();
}

jobject JNICALL NativeWindow_getSurface(JNIEnv* env, jclass, jlong ptr) {
  return android::android_view_Surface_createFromSurface(
      env, android::sp<android::Surface>::fromExisting(Get(ptr)));
}

jint JNICALL NativeWindow_getWidth(JNIEnv*, jclass, jlong ptr) {
  return Get(ptr)->window()->width();
}

jint JNICALL NativeWindow_getHeight(JNIEnv*, jclass, jlong ptr) {
  return Get(ptr)->window()->height();
}

jlong JNICALL NativeWindow_getFramesPresented(JNIEnv*, jclass, jlong ptr) {
  return static_cast<jlong>(Get(ptr)->frames());
}

jlong JNICALL NativeWindow_getHwnd(JNIEnv*, jclass, jlong ptr) {
  return reinterpret_cast<jlong>(Get(ptr)->window()->hwnd());
}

void JNICALL NativeWindow_present(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->Present();
}

// The client area as ARGB colors (GDI's BGRA little-endian words are
// 0xAARRGGBB already; GDI leaves alpha undefined, so it is set opaque):
// what PrintWindow reads back (print = true), or the back buffer. Null when
// PrintWindow fails.
jintArray JNICALL NativeWindow_capture(JNIEnv* env, jclass, jlong ptr, jboolean print) {
  std::vector<uint32_t> pixels;
  int w = 0, h = 0;
  if (print) {
    if (!Get(ptr)->window()->PrintClient(&pixels, &w, &h)) return nullptr;
  } else {
    Get(ptr)->window()->ReadBackBuffer(&pixels, &w, &h);
  }
  for (uint32_t& p : pixels) p |= 0xff000000u;
  // [width, height, pixels...]
  jintArray out = env->NewIntArray(static_cast<jsize>(pixels.size() + 2));
  if (out == nullptr) return nullptr;
  jint size[2] = {w, h};
  env->SetIntArrayRegion(out, 0, 2, size);
  env->SetIntArrayRegion(out, 2, static_cast<jsize>(pixels.size()),
                         reinterpret_cast<const jint*>(pixels.data()));
  return out;
}

// ---- Popups (gesher_popup.cc; docs/graphics.md, "Dialogs, menus and other
// popups") ----

gesher::Window* WindowOf(jlong ptr) {
  return ptr != 0 ? Get(ptr)->window() : nullptr;
}

jlong JNICALL NativeWindow_createPopup(JNIEnv* env, jclass, jstring title, jint width,
                                       jint height, jlong owner) {
  gesher::Window* window =
      gesher::Window::CreatePopup(Utf8(env, title), width, height, WindowOf(owner));
  if (window == nullptr) return 0;
  auto* surface = new Win32Surface(window);
  surface->incStrong(reinterpret_cast<void*>(&NativeWindow_create));
  {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    gOpen.insert(surface);
  }
  return reinterpret_cast<jlong>(surface);
}

void JNICALL NativeWindow_setPopupBounds(JNIEnv*, jclass, jlong ptr, jlong anchor, jint x,
                                         jint y, jint width, jint height) {
  Get(ptr)->window()->SetPopupBounds(WindowOf(anchor), x, y, width, height);
}

void JNICALL NativeWindow_showPopup(JNIEnv*, jclass, jlong ptr, jboolean activate) {
  Get(ptr)->window()->ShowPopup(activate);
}

void JNICALL NativeWindow_setTranslucent(JNIEnv*, jclass, jlong ptr, jboolean translucent) {
  Get(ptr)->window()->SetTranslucent(translucent);
}

void JNICALL NativeWindow_setNoActivate(JNIEnv*, jclass, jlong ptr, jboolean no_activate,
                                        jboolean passthrough) {
  Get(ptr)->window()->SetNoActivate(no_activate, passthrough);
}

void JNICALL NativeWindow_raise(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->window()->Raise();
}

void JNICALL NativeWindow_activate(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->window()->Activate();
}

void JNICALL NativeWindow_setDim(JNIEnv*, jclass, jlong ptr, jfloat amount) {
  Get(ptr)->window()->SetDim(amount);
}

void JNICALL NativeWindow_setInputRoute(JNIEnv* env, jclass, jlong ptr, jlong pointer, jlong keys,
                                        jlongArray outside) {
  std::vector<gesher::Window*> watchers;
  if (outside != nullptr) {
    jsize n = env->GetArrayLength(outside);
    std::vector<jlong> ptrs(n);
    env->GetLongArrayRegion(outside, 0, n, ptrs.data());
    for (jlong p : ptrs) {
      if (p != 0) watchers.push_back(WindowOf(p));
    }
  }
  Get(ptr)->window()->SetInputRoute(WindowOf(pointer), WindowOf(keys), watchers);
}

// The client area in screen coordinates: {left, top, right, bottom}.
jintArray JNICALL NativeWindow_getScreenBounds(JNIEnv* env, jclass, jlong ptr) {
  int r[4];
  Get(ptr)->window()->ScreenBounds(r);
  jintArray out = env->NewIntArray(4);
  if (out == nullptr) return nullptr;
  jint v[4] = {r[0], r[1], r[2], r[3]};
  env->SetIntArrayRegion(out, 0, 4, v);
  return out;
}

void JNICALL NativeWindow_setScreenPosition(JNIEnv*, jclass, jlong ptr, jint x, jint y) {
  Get(ptr)->window()->SetScreenPosition(x, y);
}

const JNINativeMethod kMethods[] = {
    {"nCreate", "(Ljava/lang/String;II)J", reinterpret_cast<void*>(&NativeWindow_create)},
    {"nDestroy", "(J)V", reinterpret_cast<void*>(&NativeWindow_destroy)},
    {"nShow", "(J)V", reinterpret_cast<void*>(&NativeWindow_show)},
    {"nShowOffscreen", "(J)V", reinterpret_cast<void*>(&NativeWindow_showOffscreen)},
    {"nGetSurface", "(J)Landroid/view/Surface;", reinterpret_cast<void*>(&NativeWindow_getSurface)},
    {"nGetWidth", "(J)I", reinterpret_cast<void*>(&NativeWindow_getWidth)},
    {"nGetHeight", "(J)I", reinterpret_cast<void*>(&NativeWindow_getHeight)},
    {"nGetFramesPresented", "(J)J", reinterpret_cast<void*>(&NativeWindow_getFramesPresented)},
    {"nGetHwnd", "(J)J", reinterpret_cast<void*>(&NativeWindow_getHwnd)},
    {"nCapture", "(JZ)[I", reinterpret_cast<void*>(&NativeWindow_capture)},
    {"nPresent", "(J)V", reinterpret_cast<void*>(&NativeWindow_present)},
    {"nHide", "(J)V", reinterpret_cast<void*>(&NativeWindow_hide)},
    {"nSetTitle", "(JLjava/lang/String;)V", reinterpret_cast<void*>(&NativeWindow_setTitle)},
    {"nSetResizable", "(JZ)V", reinterpret_cast<void*>(&NativeWindow_setResizable)},
    {"nSetClientSize", "(JII)V", reinterpret_cast<void*>(&NativeWindow_setClientSize)},
    {"nSetMinClientSize", "(JII)V", reinterpret_cast<void*>(&NativeWindow_setMinClientSize)},
    {"nTagSurfaceControl", "(JJ)V", reinterpret_cast<void*>(&NativeWindow_tagSurfaceControl)},
    {"nGetEventFd", "(J)I", reinterpret_cast<void*>(&NativeWindow_getEventFd)},
    {"nPollEvent", "(J[D)I", reinterpret_cast<void*>(&NativeWindow_pollEvent)},
    {"nSetIcon", "(JLjava/lang/String;)Z", reinterpret_cast<void*>(&NativeWindow_setIcon)},
    {"nGetAppUserModelId", "(J)Ljava/lang/String;",
     reinterpret_cast<void*>(&NativeWindow_getAppUserModelId)},
    // Popups.
    {"nCreatePopup", "(Ljava/lang/String;IIJ)J", reinterpret_cast<void*>(&NativeWindow_createPopup)},
    {"nSetPopupBounds", "(JJIIII)V", reinterpret_cast<void*>(&NativeWindow_setPopupBounds)},
    {"nShowPopup", "(JZ)V", reinterpret_cast<void*>(&NativeWindow_showPopup)},
    {"nSetTranslucent", "(JZ)V", reinterpret_cast<void*>(&NativeWindow_setTranslucent)},
    {"nSetNoActivate", "(JZZ)V", reinterpret_cast<void*>(&NativeWindow_setNoActivate)},
    {"nRaise", "(J)V", reinterpret_cast<void*>(&NativeWindow_raise)},
    {"nActivate", "(J)V", reinterpret_cast<void*>(&NativeWindow_activate)},
    {"nSetDim", "(JF)V", reinterpret_cast<void*>(&NativeWindow_setDim)},
    {"nSetInputRoute", "(JJJ[J)V", reinterpret_cast<void*>(&NativeWindow_setInputRoute)},
    {"nGetScreenBounds", "(J)[I", reinterpret_cast<void*>(&NativeWindow_getScreenBounds)},
    {"nSetScreenPosition", "(JII)V", reinterpret_cast<void*>(&NativeWindow_setScreenPosition)},
};

}  // namespace

// The NDK's ANativeWindow_toSurface (gesher_native_window.h): a Java Surface
// over a Surface-type ANativeWindow (a Win32Surface or another hostgraphics
// Surface).
extern "C" jobject ANativeWindow_toSurface(JNIEnv* env, ANativeWindow* window) {
  if (window == nullptr) return nullptr;
  int type = -1;
  window->query(window, NATIVE_WINDOW_CONCRETE_TYPE, &type);
  if (type != NATIVE_WINDOW_SURFACE) return nullptr;
  return android::android_view_Surface_createFromSurface(
      env, android::sp<android::Surface>::fromExisting(static_cast<android::Surface*>(window)));
}

int register_com_gesher_app_window_NativeWindow(JNIEnv* env) {
  // ViewRootImpl's Surface comes from BLASTBufferQueue.nativeGetSurface.
  gesher_set_blast_surface_provider(&BlastSurface);
  return jniRegisterNativeMethods(env, "com/gesher/app/window/NativeWindow", kMethods,
                                  sizeof(kMethods) / sizeof(kMethods[0]));
}
