// Gesher: com.gesher.app.window.NativeWindow's natives (gesher-app,
// app/java/com/gesher/app/window/NativeWindow.java): a Win32 window and the
// android.view.Surface that draws into it (Win32Surface). Registered from
// compat/hwui/graphics_runtime.cc when the class is on the class path.
//
// The functions are JNICALL, which is System V under GESHER, as ART calls
// natives (docs/decisions/0006-managed-abi.md).

#include <jni.h>
#include <string.h>

#include <vector>

#include <android/native_window_jni.h>
#include <android_runtime/android_view_Surface.h>
#include <nativehelper/JNIHelp.h>
#include <system/window.h>

#include "gesher_window.h"
#include "win32_surface.h"

using gesher::Win32Surface;

namespace {

Win32Surface* Get(jlong ptr) {
  return reinterpret_cast<Win32Surface*>(ptr);
}

jlong JNICALL NativeWindow_create(JNIEnv* env, jclass, jstring title, jint width, jint height) {
  const char* utf = title != nullptr ? env->GetStringUTFChars(title, nullptr) : nullptr;
  gesher::Window* window = gesher::Window::Create(utf != nullptr ? utf : "", width, height);
  if (utf != nullptr) env->ReleaseStringUTFChars(title, utf);
  if (window == nullptr) return 0;
  auto* surface = new Win32Surface(window);
  surface->incStrong(reinterpret_cast<void*>(&NativeWindow_create));
  return reinterpret_cast<jlong>(surface);
}

void JNICALL NativeWindow_destroy(JNIEnv*, jclass, jlong ptr) {
  Get(ptr)->decStrong(reinterpret_cast<void*>(&NativeWindow_create));
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

const JNINativeMethod kMethods[] = {
    {"nCreate", "(Ljava/lang/String;II)J", reinterpret_cast<void*>(&NativeWindow_create)},
    {"nDestroy", "(J)V", reinterpret_cast<void*>(&NativeWindow_destroy)},
    {"nShow", "(J)V", reinterpret_cast<void*>(&NativeWindow_show)},
    {"nGetSurface", "(J)Landroid/view/Surface;", reinterpret_cast<void*>(&NativeWindow_getSurface)},
    {"nGetWidth", "(J)I", reinterpret_cast<void*>(&NativeWindow_getWidth)},
    {"nGetHeight", "(J)I", reinterpret_cast<void*>(&NativeWindow_getHeight)},
    {"nGetFramesPresented", "(J)J", reinterpret_cast<void*>(&NativeWindow_getFramesPresented)},
    {"nGetHwnd", "(J)J", reinterpret_cast<void*>(&NativeWindow_getHwnd)},
    {"nCapture", "(JZ)[I", reinterpret_cast<void*>(&NativeWindow_capture)},
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
  return jniRegisterNativeMethods(env, "com/gesher/app/window/NativeWindow", kMethods,
                                  sizeof(kMethods) / sizeof(kMethods[0]));
}
