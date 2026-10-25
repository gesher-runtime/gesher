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
// Translucent popups (lifecycle track, docs/lifecycle.md, ADR 0048): a
// popup whose format has alpha is a layered window, which a swap chain cannot
// present to. Its "window surface" is a pbuffer of the window's size behind a
// handle of ours (LayeredSurface); each swap reads the frame back
// (premultiplied, as Skia draws it) into the window's back buffer, which the
// window presents with UpdateLayeredWindow, composed with per-pixel alpha over
// what is behind it, as the CPU pipeline's frames are. When the window's size
// changes the pbuffer is made again at the next eglMakeCurrent.
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

#include "gesher_native_window.h"

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

// Whether ANGLE's display reports the extension (ANGLE exports some entry
// points whose extension a backend lacks; calling them fails).
bool AngleExtension(EGLDisplay dpy, const char* extension) {
  const char* list = ANGLE(eglQueryString)(dpy, EGL_EXTENSIONS);
  if (list == nullptr) return false;
  std::string padded = std::string(" ") + list + " ";
  return padded.find(std::string(" ") + extension + " ") != std::string::npos;
}

// --- translucent popups: pbuffers read back into layered windows -----------------------------------

struct LayeredSurface {
  EGLSurface pbuffer = EGL_NO_SURFACE;
  EGLConfig config = nullptr;
  ANativeWindow* window = nullptr;
  EGLint width = 0, height = 0;
};

std::mutex g_layered_mutex;
std::unordered_map<void*, LayeredSurface*>& Layered() {
  static auto* map = new std::unordered_map<void*, LayeredSurface*>();
  return *map;
}

LayeredSurface* LayeredOf(EGLSurface surface) {
  if (surface == EGL_NO_SURFACE) return nullptr;
  std::lock_guard<std::mutex> lock(g_layered_mutex);
  auto it = Layered().find(surface);
  return it != Layered().end() ? it->second : nullptr;
}

// The pbuffer for the window's size now (made again when it changed).
EGLSurface LayeredPbuffer(EGLDisplay dpy, LayeredSurface* s) {
  int32_t w = 0, h = 0;
  gesher_window_client_size(s->window, &w, &h);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (s->pbuffer != EGL_NO_SURFACE && w == s->width && h == s->height) return s->pbuffer;
  const EGLint attribs[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
  EGLSurface pbuffer = ANGLE(eglCreatePbufferSurface)(dpy, s->config, attribs);
  if (pbuffer == EGL_NO_SURFACE) return s->pbuffer;  // keep the old size
  if (s->pbuffer != EGL_NO_SURFACE) ANGLE(eglDestroySurface)(dpy, s->pbuffer);
  s->pbuffer = pbuffer;
  s->width = w;
  s->height = h;
  return pbuffer;
}

// The ANGLE surface behind `surface` (a layered surface's pbuffer).
EGLSurface Real(EGLSurface surface) {
  LayeredSurface* s = LayeredOf(surface);
  return s != nullptr ? s->pbuffer : surface;
}

// Reads the frame back from the current draw surface (the pbuffer) into the
// window, which presents it as a layered window.
void PresentLayered(LayeredSurface* s) {
  const EGLint w = s->width, h = s->height;
  if (w < 1 || h < 1) return;
  GLint read_fb = 0, draw_fb = 0;
  ANGLE(glGetIntegerv)(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  ANGLE(glGetIntegerv)(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
  ANGLE(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  ANGLE(glReadPixels)(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  ANGLE(glBindFramebuffer)(GL_READ_FRAMEBUFFER, read_fb);
  ANGLE(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, draw_fb);
  std::vector<uint32_t> argb(static_cast<size_t>(w) * h);
  for (EGLint y = 0; y < h; y++) {
    const uint8_t* row = rgba.data() + static_cast<size_t>(h - 1 - y) * w * 4;  // bottom-up
    uint32_t* out = argb.data() + static_cast<size_t>(y) * w;
    for (EGLint x = 0; x < w; x++) {
      const uint8_t* p = row + x * 4;
      out[x] = (uint32_t{p[3]} << 24) | (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
    }
  }
  gesher_window_present_pixels(s->window, argb.data(), w, h);
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
    if (!AngleExtension(dpy, "EGL_KHR_swap_buffers_with_damage")) {
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
  // A layered surface's pbuffer follows its window's size.
  if (LayeredSurface* s = LayeredOf(draw)) draw = LayeredPbuffer(dpy, s);
  if (LayeredSurface* s = LayeredOf(read)) read = LayeredPbuffer(dpy, s);
  return ANGLE(eglMakeCurrent)(dpy, draw, read, ctx);
}

EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                               const EGLint* attribs) {
  return ANGLE(eglCreatePbufferSurface)(dpy, config, attribs);
}

EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                              EGLNativeWindowType window, const EGLint* attribs) {
  auto* anw = static_cast<ANativeWindow*>(window);
  if (anw != nullptr && gesher_window_translucent(anw)) {
    // A translucent popup (a layered window): a pbuffer read back at each swap.
    auto* s = new LayeredSurface();
    s->config = config;
    s->window = anw;
    if (LayeredPbuffer(dpy, s) == EGL_NO_SURFACE) {
      delete s;
      return EGL_NO_SURFACE;
    }
    ANativeWindow_acquire(anw);
    std::lock_guard<std::mutex> lock(g_layered_mutex);
    Layered()[s] = s;
    return static_cast<EGLSurface>(s);
  }
  return gesher_egl_create_window_surface(dpy, config, anw, attribs);
}

EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
  LayeredSurface* s = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_layered_mutex);
    auto it = Layered().find(surface);
    if (it != Layered().end()) {
      s = it->second;
      Layered().erase(it);
    }
  }
  if (s != nullptr) {
    EGLBoolean ok = s->pbuffer != EGL_NO_SURFACE ? ANGLE(eglDestroySurface)(dpy, s->pbuffer)
                                                 : EGL_TRUE;
    ANativeWindow_release(s->window);
    delete s;
    return ok;
  }
  return gesher_egl_destroy_surface(dpy, surface);
}

EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                                       EGLint* value) {
  if (LayeredOf(surface) != nullptr && attribute == EGL_BUFFER_AGE_EXT) {
    *value = 0;  // each frame is drawn whole
    return EGL_TRUE;
  }
  return ANGLE(eglQuerySurface)(dpy, Real(surface), attribute, value);
}

EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                                        EGLint value) {
  if (LayeredOf(surface) != nullptr) {
    // A pbuffer's contents are its own between frames: any swap behavior holds.
    ANGLE(eglSurfaceAttrib)(dpy, Real(surface), attribute, value);
    return EGL_TRUE;
  }
  return ANGLE(eglSurfaceAttrib)(dpy, surface, attribute, value);
}

EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval) {
  return ANGLE(eglSwapInterval)(dpy, interval);
}

EGLBoolean EGLAPIENTRY eglSetDamageRegionKHR(EGLDisplay dpy, EGLSurface surface, EGLint* rects,
                                             EGLint n_rects) {
  if (LayeredOf(surface) != nullptr) return EGL_TRUE;  // the whole frame is read back
  return ANGLE(eglSetDamageRegionKHR)(dpy, surface, rects, n_rects);
}

EGLBoolean EGLAPIENTRY eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface,
                                                   const EGLint* rects, EGLint n_rects) {
  if (LayeredSurface* s = LayeredOf(surface)) {
    PresentLayered(s);
    return EGL_TRUE;
  }
  static const bool has_damage = AngleExtension(dpy, "EGL_KHR_swap_buffers_with_damage");
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

// Android-only: marks the start of a frame for systrace. EglManager.cpp
// declares it itself, outside extern "C".
void EGLAPIENTRY eglBeginFrame(EGLDisplay, EGLSurface) {}

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
    sk_sp<const GrGLInterface> gl_interface = GrGLMakeNativeInterface();
    ok = renderer != nullptr && version != nullptr && gl_interface != nullptr &&
         gl_interface->validate();
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
