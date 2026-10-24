/*
 * Gesher: Android's EGL meanings over ANGLE, for code inside dalvikvm
 * (compat/bionic/egl_gles.cc; ADR 0023 and 0030).
 *
 * egl_gles.cc gives an app's .so files (System V) Android's EGL on ANGLE:
 * the default display is ANGLE's Direct3D 11 device (hardware, else WARP;
 * GESHER_EGL_DEVICE=warp), a window surface takes an ANativeWindow (a Gesher
 * window's HWND gets ANGLE's swap chain, and the window's own WM_PAINT is
 * switched off while it exists), and Android-only config attributes are
 * handled. These functions offer the same to Microsoft-ABI code in the
 * executable (hwui's EglManager, through compat/hwui/gl/angle_gl.cc). The
 * EGL types are ANGLE's (void* handles).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

/* ANGLE's entry point `name` (libEGL.dll/libGLESv2.dll next to the
 * executable, else eglGetProcAddress), Microsoft x64 ABI; null when ANGLE
 * is missing or does not have it. */
void* gesher_egl_proc(const char* name);

/* Android's EGL_DEFAULT_DISPLAY: ANGLE's Direct3D 11 display, initialized;
 * EGL_NO_DISPLAY (null) without ANGLE or a device. */
void* gesher_egl_default_display(void);

/* eglChooseConfig / eglGetConfigAttrib with Android's attributes
 * (EGL_RECORDABLE_ANDROID, EGL_FRAMEBUFFER_TARGET_ANDROID,
 * EGL_NATIVE_VISUAL_ID as a HAL pixel format). */
unsigned gesher_egl_choose_config(void* display, const int32_t* attribs, void** configs,
                                  int32_t config_size, int32_t* num_config);
unsigned gesher_egl_get_config_attrib(void* display, void* config, int32_t attribute,
                                      int32_t* value);

/* eglCreateWindowSurface on an ANativeWindow: a Gesher window's HWND gets
 * ANGLE's window surface, holding a reference to the window until
 * gesher_egl_destroy_surface; other windows give EGL_NO_SURFACE
 * (EGL_BAD_NATIVE_WINDOW). */
void* gesher_egl_create_window_surface(void* display, void* config, struct ANativeWindow* window,
                                       const int32_t* attribs);

/* eglDestroySurface, giving back what gesher_egl_create_window_surface took. */
unsigned gesher_egl_destroy_surface(void* display, void* surface);

/* The HWND of a window surface made by gesher_egl_create_window_surface, or
 * null. */
void* gesher_egl_surface_hwnd(void* surface);

#ifdef __cplusplus
}
#endif
