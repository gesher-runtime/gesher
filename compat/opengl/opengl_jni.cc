// Gesher: the framework's Java OpenGL ES and EGL bindings over ANGLE
// (docs/angle.md, "Java apps": com.google.android.gles_jni's EGL10/GL10,
// android.opengl's EGL14/15, GLES10-32, GLUtils, ETC1, Visibility).
//
// cmake/aosp/opengl.cmake builds core/jni's sources for them as they are
// (their JNI tables System V: frameworks/base patch 0760); they call
// eglXxx/glXxx, which the executable defines:
//  - compat/hwui/gl/angle_gl.cc: what hwui calls, with Android's meanings
//    (the one display, configs with Android's attributes, window surfaces on
//    an ANativeWindow: a Gesher window's HWND gets ANGLE's swap chain, a
//    SurfaceView's or a SurfaceTexture's surface a hidden helper window whose
//    frames are posted to it, egl_child_surface.cc);
//  - this file: the rest of Android's meanings (below);
//  - gl_forwarders.cc (gen_gl_forwarders.py): every other function, straight
//    to ANGLE's entry point.
//
// Here:
//  - eglSwapBuffers(WithDamageEXT): a child surface posts its frame first;
//  - eglGetPlatformDisplay(EXT) for EGL_PLATFORM_ANDROID_KHR is the default
//    display, and eglCreatePlatformWindowSurface(EXT) takes an ANativeWindow;
//  - egl_get_init_count (libEGL's private API, EGLImpl.getInitCount);
//  - a window surface on a SurfaceTexture (EGL10's and EGL14's
//    eglCreateWindowSurface with one): the device makes a Surface on its
//    BufferQueue producer; here the Java Surface over it (whose window is the
//    media track's QueueSurface, compat/media/surface) goes to the Surface
//    path, so libgui's producer (SurfaceTexture_getProducer) is never asked for.

#include "gl_forwarders.h"

#include <android/native_window.h>
#include <android_runtime/android_graphics_SurfaceTexture.h>
#include <android_runtime/android_view_Surface.h>
#include <gesher/egl_angle.h>
#include <gui/IGraphicBufferProducer.h>
#include <private/EGL/display.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core_jni_helpers.h"

extern int register_com_google_android_gles_jni_EGLImpl(JNIEnv* env);
extern int register_com_google_android_gles_jni_GLImpl(JNIEnv* env);
extern int register_android_opengl_jni_EGL14(JNIEnv* env);
extern int register_android_opengl_jni_EGL15(JNIEnv* env);
extern int register_android_opengl_jni_EGLExt(JNIEnv* env);
extern int register_android_opengl_jni_GLES10(JNIEnv* env);
extern int register_android_opengl_jni_GLES10Ext(JNIEnv* env);
extern int register_android_opengl_jni_GLES11(JNIEnv* env);
extern int register_android_opengl_jni_GLES11Ext(JNIEnv* env);
extern int register_android_opengl_jni_GLES20(JNIEnv* env);
extern int register_android_opengl_jni_GLES30(JNIEnv* env);
extern int register_android_opengl_jni_GLES31(JNIEnv* env);
extern int register_android_opengl_jni_GLES31Ext(JNIEnv* env);
extern int register_android_opengl_jni_GLES32(JNIEnv* env);
namespace android {
extern int register_android_opengl_classes(JNIEnv* env);
}

void* gesher_gl_forwarder_resolve(const char* name) {
  void* p = gesher_egl_proc(name);
  if (p == nullptr) {
    std::fprintf(stderr, "gesher OpenGL ES: ANGLE does not provide %s\n", name);
    std::fflush(stderr);
    std::abort();
  }
  return p;
}

namespace {

template <typename F>
F Angle(const char* name) {
  return reinterpret_cast<F>(gesher_gl_forwarder_resolve(name));
}

std::vector<EGLint> ToEglInt(const EGLAttrib* attribs) {
  std::vector<EGLint> list;
  if (attribs != nullptr) {
    for (const EGLAttrib* a = attribs; *a != EGL_NONE; a += 2) {
      list.push_back(static_cast<EGLint>(a[0]));
      list.push_back(static_cast<EGLint>(a[1]));
    }
  }
  list.push_back(EGL_NONE);
  return list;
}

}  // namespace

extern "C" {

EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  return eglSwapBuffersWithDamageKHR(dpy, surface, nullptr, 0);
}

EGLBoolean EGLAPIENTRY eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface,
                                                   const EGLint* rects, EGLint n_rects) {
  return eglSwapBuffersWithDamageKHR(dpy, surface, rects, n_rects);
}

EGLDisplay EGLAPIENTRY eglGetPlatformDisplay(EGLenum platform, void* native_display,
                                             const EGLAttrib* attribs) {
  if (platform == EGL_PLATFORM_ANDROID_KHR) return gesher_egl_default_display();
  static auto get = Angle<decltype(&::eglGetPlatformDisplay)>("eglGetPlatformDisplay");
  return get(platform, native_display, attribs);
}

EGLDisplay EGLAPIENTRY eglGetPlatformDisplayEXT(EGLenum platform, void* native_display,
                                                const EGLint* attribs) {
  if (platform == EGL_PLATFORM_ANDROID_KHR) return gesher_egl_default_display();
  static auto get = Angle<decltype(&::eglGetPlatformDisplayEXT)>("eglGetPlatformDisplayEXT");
  return get(platform, native_display, attribs);
}

// For EGL_PLATFORM_ANDROID_KHR the native window is the ANativeWindow*.
EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config,
                                                      void* native_window,
                                                      const EGLAttrib* attribs) {
  std::vector<EGLint> list = ToEglInt(attribs);
  return eglCreateWindowSurface(dpy, config, static_cast<EGLNativeWindowType>(native_window),
                                list.data());
}

EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config,
                                                         void* native_window,
                                                         const EGLint* attribs) {
  return eglCreateWindowSurface(dpy, config, static_cast<EGLNativeWindowType>(native_window),
                                attribs);
}

// libGLESv1_CM's private entries for GL10's buffers (frameworks/native
// opengl/libs/GLES_CM/gl.cpp): the element count is for bounds checks, which
// the plain functions do without.
void GL_APIENTRY glColorPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid* ptr,
                                      GLsizei) {
  glColorPointer(size, type, stride, ptr);
}

void GL_APIENTRY glNormalPointerBounds(GLenum type, GLsizei stride, const GLvoid* ptr, GLsizei) {
  glNormalPointer(type, stride, ptr);
}

void GL_APIENTRY glTexCoordPointerBounds(GLint size, GLenum type, GLsizei stride,
                                         const GLvoid* ptr, GLsizei) {
  glTexCoordPointer(size, type, stride, ptr);
}

void GL_APIENTRY glVertexPointerBounds(GLint size, GLenum type, GLsizei stride, const GLvoid* ptr,
                                       GLsizei) {
  glVertexPointer(size, type, stride, ptr);
}

void GL_APIENTRY glPointSizePointerOESBounds(GLenum type, GLsizei stride, const GLvoid* ptr,
                                             GLsizei) {
  glPointSizePointerOES(type, stride, ptr);
}

void GL_APIENTRY glMatrixIndexPointerOESBounds(GLint size, GLenum type, GLsizei stride,
                                               const GLvoid* ptr, GLsizei) {
  glMatrixIndexPointerOES(size, type, stride, ptr);
}

void GL_APIENTRY glWeightPointerOESBounds(GLint size, GLenum type, GLsizei stride,
                                          const GLvoid* ptr, GLsizei) {
  glWeightPointerOES(size, type, stride, ptr);
}

}  // extern "C"

namespace android {

// How many times the display was initialized and not terminated; ANGLE does
// not count, and Android's default display is initialized once for good.
EGLint egl_get_init_count(EGLDisplay dpy) {
  return dpy != EGL_NO_DISPLAY ? 1 : 0;
}

// Not reached: the SurfaceTexture paths go through a Surface (below).
sp<IGraphicBufferProducer> SurfaceTexture_getProducer(JNIEnv*, jobject) {
  return nullptr;
}

}  // namespace android

namespace {

// A Surface over `surface_texture` (its producer, a QueueSurface), or null
// with an exception pending.
jobject SurfaceOver(JNIEnv* env, jobject surface_texture) {
  jclass surface_class = env->FindClass("android/view/Surface");
  jmethodID ctor =
      env->GetMethodID(surface_class, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
  jobject surface = env->NewObject(surface_class, ctor, surface_texture);
  env->DeleteLocalRef(surface_class);
  return surface;
}

void Release(JNIEnv* env, jobject surface) {
  jclass surface_class = env->GetObjectClass(surface);
  env->CallVoidMethod(surface, env->GetMethodID(surface_class, "release", "()V"));
  env->DeleteLocalRef(surface_class);
  env->DeleteLocalRef(surface);
}

#define EGL10_DISPLAY "Ljavax/microedition/khronos/egl/EGLDisplay;"
#define EGL10_CONFIG "Ljavax/microedition/khronos/egl/EGLConfig;"

// EGLImpl._eglCreateWindowSurfaceTexture: _eglCreateWindowSurface with a Surface
// over the SurfaceTexture. The EGL surface holds its window (egl_gles.cc).
jlong EglImpl_createWindowSurfaceTexture(JNIEnv* env, jobject thiz, jobject display,
                                         jobject config, jobject surface_texture,
                                         jintArray attribs) {
  if (surface_texture == nullptr) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "Make sure the SurfaceTexture is valid");
    return 0;
  }
  jobject surface = SurfaceOver(env, surface_texture);
  if (surface == nullptr) return 0;
  jclass clazz = env->GetObjectClass(thiz);
  jmethodID create = env->GetMethodID(clazz, "_eglCreateWindowSurface",
                                      "(" EGL10_DISPLAY EGL10_CONFIG "Ljava/lang/Object;[I)J");
  env->DeleteLocalRef(clazz);
  jlong result = env->CallLongMethod(thiz, create, display, config, surface, attribs);
  Release(env, surface);
  return result;
}

#define EGL14_DISPLAY "Landroid/opengl/EGLDisplay;"
#define EGL14_CONFIG "Landroid/opengl/EGLConfig;"
#define EGL14_SURFACE "Landroid/opengl/EGLSurface;"

// EGL14._eglCreateWindowSurfaceTexture: the same for EGL14.
jobject Egl14_createWindowSurfaceTexture(JNIEnv* env, jclass clazz, jobject display,
                                         jobject config, jobject surface_texture,
                                         jintArray attribs, jint offset) {
  if (surface_texture == nullptr) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "Make sure the SurfaceView or associated SurfaceHolder has a valid Surface");
    return nullptr;
  }
  jobject surface = SurfaceOver(env, surface_texture);
  if (surface == nullptr) return nullptr;
  jmethodID create = env->GetStaticMethodID(
      clazz, "_eglCreateWindowSurface",
      "(" EGL14_DISPLAY EGL14_CONFIG "Ljava/lang/Object;[II)" EGL14_SURFACE);
  jobject result = env->CallStaticObjectMethod(clazz, create, display, config, surface, attribs,
                                               offset);
  Release(env, surface);
  return result;
}

const JNINativeMethod kEglImplMethods[] = {
    {"_eglCreateWindowSurfaceTexture", "(" EGL10_DISPLAY EGL10_CONFIG "Ljava/lang/Object;[I)J",
     GESHER_JNI(EglImpl_createWindowSurfaceTexture)},
};

const JNINativeMethod kEgl14Methods[] = {
    {"_eglCreateWindowSurfaceTexture",
     "(" EGL14_DISPLAY EGL14_CONFIG "Ljava/lang/Object;[II)" EGL14_SURFACE,
     GESHER_JNI(Egl14_createWindowSurfaceTexture)},
};

bool HasClass(JNIEnv* env, const char* name) {
  jclass c = env->FindClass(name);
  if (c == nullptr) {
    env->ExceptionClear();
    return false;
  }
  env->DeleteLocalRef(c);
  return true;
}

}  // namespace

// AndroidRuntime.cpp's registrations of the bindings, in its order, each when
// its class is on the class path; then the SurfaceTexture paths over them.
int register_gesher_opengl(JNIEnv* env) {
  static const struct {
    const char* className;
    int (*registerFunction)(JNIEnv*);
  } kRegistrations[] = {
      {"com/google/android/gles_jni/EGLImpl", register_com_google_android_gles_jni_EGLImpl},
      {"com/google/android/gles_jni/GLImpl", register_com_google_android_gles_jni_GLImpl},
      {"android/opengl/EGL14", register_android_opengl_jni_EGL14},
      {"android/opengl/EGL15", register_android_opengl_jni_EGL15},
      {"android/opengl/EGLExt", register_android_opengl_jni_EGLExt},
      {"android/opengl/GLES10", register_android_opengl_jni_GLES10},
      {"android/opengl/GLES10Ext", register_android_opengl_jni_GLES10Ext},
      {"android/opengl/GLES11", register_android_opengl_jni_GLES11},
      {"android/opengl/GLES11Ext", register_android_opengl_jni_GLES11Ext},
      {"android/opengl/GLES20", register_android_opengl_jni_GLES20},
      {"android/opengl/GLES30", register_android_opengl_jni_GLES30},
      {"android/opengl/GLES31", register_android_opengl_jni_GLES31},
      {"android/opengl/GLES31Ext", register_android_opengl_jni_GLES31Ext},
      {"android/opengl/GLES32", register_android_opengl_jni_GLES32},
      {"android/opengl/GLUtils", android::register_android_opengl_classes},
  };
  for (const auto& r : kRegistrations) {
    if (HasClass(env, r.className) && r.registerFunction(env) < 0) return -1;
  }
  if (HasClass(env, "com/google/android/gles_jni/EGLImpl")) {
    android::RegisterMethodsOrDie(env, "com/google/android/gles_jni/EGLImpl", kEglImplMethods,
                                  NELEM(kEglImplMethods));
  }
  if (HasClass(env, "android/opengl/EGL14")) {
    android::RegisterMethodsOrDie(env, "android/opengl/EGL14", kEgl14Methods,
                                  NELEM(kEgl14Methods));
  }
  return 0;
}
