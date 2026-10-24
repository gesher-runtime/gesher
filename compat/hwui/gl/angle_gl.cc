// Gesher: the EGL and OpenGL ES functions hwui calls itself (EglManager,
// SkiaOpenGLPipeline, GLUtils), over ANGLE (docs/angle.md, ADR 0030).
//
// dalvikvm does not link ANGLE's import libraries (it starts without the
// DLLs; ADR 0023): these definitions resolve ANGLE's entry point on first
// call through compat/bionic/egl_gles.cc (gesher/egl_angle.h), which also
// gives them Android's meanings, as an app's .so gets them:
//  - eglGetDisplay(anything) is Android's one display: ANGLE's Direct3D 11
//    device, hardware else WARP (GESHER_EGL_DEVICE=warp asks for WARP);
//  - eglCreateWindowSurface takes an ANativeWindow (hwui's Surface, a
//    Win32Surface) and puts ANGLE's swap chain on the Gesher window's HWND,
//    whose own WM_PAINT is off while the surface exists;
//  - eglChooseConfig/eglGetConfigAttrib know Android's attributes.
// What Android's EGL has and ANGLE does not:
//  - eglBeginFrame (a systrace marker) does nothing;
//  - EGL_KHR_swap_buffers_with_damage, which EglManager requires, is
//    reported and served by eglSwapBuffers when ANGLE lacks it;
//  - eglDupNativeFenceFDANDROID has no Android fences to give (ANGLE does
//    not report EGL_ANDROID_native_fence_sync, so EglManager does not call it).
// Skia's Ganesh reaches GLES through its GrGLInterface, not through these.
//
// gesher_hwui_gl_usable() is the check that decides whether the SkiaGL
// pipeline can be used at all (pipeline_select.cc): EglManager aborts the
// process when EGL fails, so it is tried here first.

#include "angle_gl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include <include/gpu/ganesh/gl/GrGLInterface.h>

#include <gesher/egl_angle.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

void* Resolve(const char* name) {
  void* p = gesher_egl_proc(name);
  if (p == nullptr) {
    std::fprintf(stderr, "gesher hwui GL: ANGLE does not provide %s\n", name);
    std::fflush(stderr);
    std::abort();
  }
  return p;
}

// ANGLE's `name`, typed as F, resolved once.
template <typename F>
F Angle(std::atomic<void*>& slot, const char* name) {
  void* p = slot.load(std::memory_order_acquire);
  if (p == nullptr) {
    p = Resolve(name);
    slot.store(p, std::memory_order_release);
  }
  return reinterpret_cast<F>(p);
}

#define ANGLE(name)                         \
  ([]() {                                   \
    static std::atomic<void*> slot{nullptr}; \
    return Angle<decltype(&::name)>(slot, #name); \
  }())

bool AngleHas(const char* name) {
  return gesher_egl_proc(name) != nullptr;
}

}  // namespace

extern "C" {

// --- displays and configs --------------------------------------------------------------------------

EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType) {
  return gesher_egl_default_display();
}

EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
  return ANGLE(eglInitialize)(dpy, major, minor);
}

EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy) {
  return ANGLE(eglTerminate)(dpy);
}

EGLBoolean EGLAPIENTRY eglReleaseThread(void) {
  return ANGLE(eglReleaseThread)();
}

EGLint EGLAPIENTRY eglGetError(void) {
  return ANGLE(eglGetError)();
}

const char* EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name) {
  const char* value = ANGLE(eglQueryString)(dpy, name);
  if (name != EGL_EXTENSIONS || value == nullptr) return value;
  // EglManager requires EGL_KHR_swap_buffers_with_damage; eglSwapBuffers
  // serves it when ANGLE lacks it (below).
  static std::mutex mutex;
  static auto* extended = new std::unordered_map<EGLDisplay, std::string>();
  std::lock_guard<std::mutex> lock(mutex);
  std::string& s = (*extended)[dpy];
  if (s.empty()) {
    s = value;
    if ((" " + s + " ").find(" EGL_KHR_swap_buffers_with_damage ") == std::string::npos) {
      s += " EGL_KHR_swap_buffers_with_damage";
    }
  }
  return s.c_str();
}

EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, const EGLint* attribs, EGLConfig* configs,
                                       EGLint config_size, EGLint* num_config) {
  return gesher_egl_choose_config(dpy, attribs, configs, config_size, num_config);
}

EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute,
                                          EGLint* value) {
  return gesher_egl_get_config_attrib(dpy, config, attribute, value);
}

// --- contexts and surfaces -------------------------------------------------------------------------

EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share,
                                        const EGLint* attribs) {
  return ANGLE(eglCreateContext)(dpy, config, share, attribs);
}

EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
  return ANGLE(eglDestroyContext)(dpy, ctx);
}

EGLBoolean EGLAPIENTRY eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute,
                                       EGLint* value) {
  return ANGLE(eglQueryContext)(dpy, ctx, attribute, value);
}

EGLContext EGLAPIENTRY eglGetCurrentContext(void) {
  return ANGLE(eglGetCurrentContext)();
}

EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void) {
  return ANGLE(eglGetCurrentDisplay)();
}

EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                      EGLContext ctx) {
  return ANGLE(eglMakeCurrent)(dpy, draw, read, ctx);
}

EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                               const EGLint* attribs) {
  return ANGLE(eglCreatePbufferSurface)(dpy, config, attribs);
}

EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                              EGLNativeWindowType window, const EGLint* attribs) {
  return gesher_egl_create_window_surface(dpy, config, static_cast<ANativeWindow*>(window),
                                          attribs);
}

EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  return gesher_egl_destroy_surface(dpy, surface);
}

EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                                       EGLint* value) {
  return ANGLE(eglQuerySurface)(dpy, surface, attribute, value);
}

EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                                        EGLint value) {
  return ANGLE(eglSurfaceAttrib)(dpy, surface, attribute, value);
}

EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval) {
  return ANGLE(eglSwapInterval)(dpy, interval);
}

// Android-only: marks the start of a frame for systrace.
EGLAPI void EGLAPIENTRY eglBeginFrame(EGLDisplay, EGLSurface) {}

EGLBoolean EGLAPIENTRY eglSetDamageRegionKHR(EGLDisplay dpy, EGLSurface surface, EGLint* rects,
                                             EGLint n_rects) {
  return ANGLE(eglSetDamageRegionKHR)(dpy, surface, rects, n_rects);
}

EGLBoolean EGLAPIENTRY eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface,
                                                   const EGLint* rects, EGLint n_rects) {
  static const bool has_damage = AngleHas("eglSwapBuffersWithDamageKHR");
  gesher_hwui_gl_before_swap(dpy, surface);
  if (has_damage) return ANGLE(eglSwapBuffersWithDamageKHR)(dpy, surface, rects, n_rects);
  return ANGLE(eglSwapBuffers)(dpy, surface);
}

// --- sync objects and images -----------------------------------------------------------------------

EGLSyncKHR EGLAPIENTRY eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint* attribs) {
  return ANGLE(eglCreateSyncKHR)(dpy, type, attribs);
}

EGLBoolean EGLAPIENTRY eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync) {
  return ANGLE(eglDestroySyncKHR)(dpy, sync);
}

EGLint EGLAPIENTRY eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags,
                                        EGLTimeKHR timeout) {
  return ANGLE(eglClientWaitSyncKHR)(dpy, sync, flags, timeout);
}

EGLint EGLAPIENTRY eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) {
  return ANGLE(eglWaitSyncKHR)(dpy, sync, flags);
}

EGLint EGLAPIENTRY eglDupNativeFenceFDANDROID(EGLDisplay, EGLSyncKHR) {
  return EGL_NO_NATIVE_FENCE_FD_ANDROID;
}

EGLImageKHR EGLAPIENTRY eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                          EGLClientBuffer buffer, const EGLint* attribs) {
  return ANGLE(eglCreateImageKHR)(dpy, ctx, target, buffer, attribs);
}

EGLBoolean EGLAPIENTRY eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image) {
  return ANGLE(eglDestroyImageKHR)(dpy, image);
}

// --- OpenGL ES -------------------------------------------------------------------------------------

const GLubyte* GL_APIENTRY glGetString(GLenum name) {
  return ANGLE(glGetString)(name);
}

GLenum GL_APIENTRY glGetError(void) {
  return ANGLE(glGetError)();
}

void GL_APIENTRY glGetIntegerv(GLenum pname, GLint* data) {
  ANGLE(glGetIntegerv)(pname, data);
}

void GL_APIENTRY glFlush(void) {
  ANGLE(glFlush)();
}

void GL_APIENTRY glBindFramebuffer(GLenum target, GLuint framebuffer) {
  ANGLE(glBindFramebuffer)(target, framebuffer);
}

void GL_APIENTRY glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
  ANGLE(glGenFramebuffers)(n, framebuffers);
}

void GL_APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
  ANGLE(glDeleteFramebuffers)(n, framebuffers);
}

GLenum GL_APIENTRY glCheckFramebufferStatus(GLenum target) {
  return ANGLE(glCheckFramebufferStatus)(target);
}

void GL_APIENTRY glBindTexture(GLenum target, GLuint texture) {
  ANGLE(glBindTexture)(target, texture);
}

void GL_APIENTRY glGenTextures(GLsizei n, GLuint* textures) {
  ANGLE(glGenTextures)(n, textures);
}

void GL_APIENTRY glDeleteTextures(GLsizei n, const GLuint* textures) {
  ANGLE(glDeleteTextures)(n, textures);
}

void GL_APIENTRY glDrawBuffers(GLsizei n, const GLenum* bufs) {
  ANGLE(glDrawBuffers)(n, bufs);
}

void GL_APIENTRY glReadBuffer(GLenum src) {
  ANGLE(glReadBuffer)(src);
}

void GL_APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                              GLenum type, void* pixels) {
  ANGLE(glReadPixels)(x, y, width, height, format, type, pixels);
}

}  // extern "C"

// --- can SkiaGL run? -------------------------------------------------------------------------------

bool gesher_hwui_gl_usable(std::string* detail) {
  EGLDisplay display = static_cast<EGLDisplay>(gesher_egl_default_display());
  if (display == EGL_NO_DISPLAY) {
    *detail = "no EGL display on ANGLE (libEGL.dll/libGLESv2.dll missing, or no Direct3D 11 device)";
    return false;
  }
  for (const char* name : {"eglCreateContext", "eglMakeCurrent", "eglSwapBuffers",
                           "eglCreatePbufferSurface", "eglSurfaceAttrib", "eglQuerySurface",
                           "glGetString", "glReadPixels"}) {
    if (!AngleHas(name)) {
      *detail = std::string("ANGLE lacks ") + name;
      return false;
    }
  }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(display, &major, &minor)) {
    *detail = "eglInitialize failed";
    return false;
  }
  // What EglManager::load8BitsConfig asks for (without a swap behavior bit).
  const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                            EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE,
                            EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_NONE};
  EGLConfig config = nullptr;
  EGLint configs = 0;
  if (!eglChooseConfig(display, attribs, &config, 1, &configs) || configs != 1) {
    *detail = "no RGBA8 window config";
    return false;
  }
  const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
  if (context == EGL_NO_CONTEXT) {
    *detail = "eglCreateContext failed";
    return false;
  }
  const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLSurface pbuffer = eglCreatePbufferSurface(display, config, pbuffer_attribs);
  bool ok = pbuffer != EGL_NO_SURFACE && eglMakeCurrent(display, pbuffer, pbuffer, context);
  if (ok) {
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    sk_sp<const GrGLInterface> interface = GrGLMakeNativeInterface();
    ok = renderer != nullptr && version != nullptr && interface != nullptr &&
         interface->validate();
    *detail = ok ? std::string(renderer) + ", " + version + ", EGL " + std::to_string(major) +
                       "." + std::to_string(minor)
                 : "no OpenGL ES interface for Skia over ANGLE";
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  } else {
    *detail = "a pbuffer with an ES context could not be made current";
  }
  if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(display, pbuffer);
  eglDestroyContext(display, context);
  return ok;
}

// --- the debug read-back ---------------------------------------------------------------------------

namespace {

struct Frame {
  int width = 0;
  int height = 0;
  std::vector<uint32_t> argb;
};

std::atomic<int> g_readback{-1};  // -1: not decided yet
std::mutex g_frames_mutex;
std::unordered_map<void*, Frame>& Frames() {
  static auto* frames = new std::unordered_map<void*, Frame>();
  return *frames;
}

bool ReadbackOn() {
  int on = g_readback.load();
  if (on < 0) {
    const char* env = std::getenv("GESHER_HWUI_GL_READBACK");
    on = env != nullptr && std::strcmp(env, "1") == 0 ? 1 : 0;
    int expected = -1;
    if (!g_readback.compare_exchange_strong(expected, on)) on = expected;
  }
  return on == 1;
}

}  // namespace

void gesher_hwui_gl_set_readback(bool on) {
  g_readback.store(on ? 1 : 0);
}

bool gesher_hwui_gl_last_frame(void* hwnd, std::vector<uint32_t>* argb, int* width, int* height) {
  std::lock_guard<std::mutex> lock(g_frames_mutex);
  auto it = Frames().find(hwnd);
  if (it == Frames().end()) return false;
  *argb = it->second.argb;
  *width = it->second.width;
  *height = it->second.height;
  return true;
}

void gesher_hwui_gl_before_swap(void* display, void* surface) {
  if (!ReadbackOn()) return;
  void* hwnd = gesher_egl_surface_hwnd(surface);
  EGLint w = 0, h = 0;
  if (hwnd == nullptr || !eglQuerySurface(display, surface, EGL_WIDTH, &w) ||
      !eglQuerySurface(display, surface, EGL_HEIGHT, &h) || w <= 0 || h <= 0) {
    return;
  }
  // The default framebuffer (the window surface), whatever Skia has bound.
  GLint read_fb = 0, draw_fb = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
  Frame frame;
  frame.width = w;
  frame.height = h;
  frame.argb.resize(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; y++) {
    const uint8_t* row = rgba.data() + static_cast<size_t>(h - 1 - y) * w * 4;  // bottom-up
    for (int x = 0; x < w; x++) {
      const uint8_t* p = row + x * 4;
      frame.argb[static_cast<size_t>(y) * w + x] =
          (uint32_t{p[3]} << 24) | (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
    }
  }
  std::lock_guard<std::mutex> lock(g_frames_mutex);
  Frames()[hwnd] = std::move(frame);
}
