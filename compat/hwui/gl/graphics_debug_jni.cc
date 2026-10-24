// Gesher: com.gesher.app.graphics.GraphicsDebug's natives (gesher-app,
// app/java/com/gesher/app/graphics/GraphicsDebug.java): which pipeline hwui
// renders with, and two ways to read a GL window back (docs/angle.md, ADR
// 0030). Registered from compat/hwui/graphics_runtime.cc when the class is on
// the class path. JNICALL is System V under GESHER, as ART calls natives.

#include <jni.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <nativehelper/JNIHelp.h>

#include "angle_gl.h"
#include "pipeline_select.h"

// The project defines NOGDI for AOSP code; the read-back uses GDI.
#undef NOGDI
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

// {width, height, ARGB colors row by row...}, as NativeWindow.capture.
jintArray ToJava(JNIEnv* env, const std::vector<uint32_t>& argb, int w, int h) {
  jintArray out = env->NewIntArray(static_cast<jsize>(argb.size() + 2));
  if (out == nullptr) return nullptr;
  jint size[2] = {w, h};
  env->SetIntArrayRegion(out, 0, 2, size);
  env->SetIntArrayRegion(out, 2, static_cast<jsize>(argb.size()),
                         reinterpret_cast<const jint*>(argb.data()));
  return out;
}

jstring JNICALL GraphicsDebug_renderer(JNIEnv* env, jclass) {
  return env->NewStringUTF(gesher_hwui_renderer_name());
}

jstring JNICALL GraphicsDebug_rendererDetail(JNIEnv* env, jclass) {
  return env->NewStringUTF(gesher_hwui_renderer_detail());
}

// What Windows composed of the window's client area: PrintWindow with
// PW_RENDERFULLCONTENT, which sees a DirectX swap chain's frames (ANGLE's),
// unlike PW_CLIENTONLY alone (WM_PRINTCLIENT, the CPU back buffer).
jintArray JNICALL GraphicsDebug_captureComposed(JNIEnv* env, jclass, jlong hwnd_value) {
  HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(hwnd_value));
  RECT rc;
  if (!IsWindow(hwnd) || !GetClientRect(hwnd, &rc)) return nullptr;
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w < 1 || h < 1) return nullptr;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  std::vector<uint32_t> argb;
  bool ok = false;
  if (bmp != nullptr) {
    HGDIOBJ old = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
    ok = PrintWindow(hwnd, dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
    GdiFlush();
    const uint32_t* p = static_cast<const uint32_t*>(bits);
    argb.assign(p, p + static_cast<size_t>(w) * h);
    SelectObject(dc, old);
    DeleteObject(bmp);
  }
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  if (!ok) return nullptr;
  for (uint32_t& c : argb) c |= 0xff000000u;  // GDI leaves alpha undefined
  return ToJava(env, argb, w, h);
}

void JNICALL GraphicsDebug_setGlReadback(JNIEnv*, jclass, jboolean on) {
  gesher_hwui_gl_set_readback(on);
}

jintArray JNICALL GraphicsDebug_lastGlFrame(JNIEnv* env, jclass, jlong hwnd) {
  std::vector<uint32_t> argb;
  int w = 0, h = 0;
  if (!gesher_hwui_gl_last_frame(reinterpret_cast<void*>(static_cast<intptr_t>(hwnd)), &argb, &w,
                                 &h)) {
    return nullptr;
  }
  return ToJava(env, argb, w, h);
}

const JNINativeMethod kMethods[] = {
    {"nRenderer", "()Ljava/lang/String;", reinterpret_cast<void*>(&GraphicsDebug_renderer)},
    {"nRendererDetail", "()Ljava/lang/String;",
     reinterpret_cast<void*>(&GraphicsDebug_rendererDetail)},
    {"nCaptureComposed", "(J)[I", reinterpret_cast<void*>(&GraphicsDebug_captureComposed)},
    {"nSetGlReadback", "(Z)V", reinterpret_cast<void*>(&GraphicsDebug_setGlReadback)},
    {"nLastGlFrame", "(J)[I", reinterpret_cast<void*>(&GraphicsDebug_lastGlFrame)},
};

}  // namespace

int register_com_gesher_app_graphics_GraphicsDebug(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "com/gesher/app/graphics/GraphicsDebug", kMethods,
                                  sizeof(kMethods) / sizeof(kMethods[0]));
}
