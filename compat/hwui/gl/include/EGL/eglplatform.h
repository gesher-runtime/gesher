/*
** Copyright 2007-2020 The Khronos Group Inc.
** SPDX-License-Identifier: Apache-2.0
*/

/* Gesher: EGL's platform types for hwui on Windows (ADR 0030), in place of
 * frameworks/native/opengl/include/EGL/eglplatform.h, whose _WIN32 branch
 * includes <windows.h> and makes the native types HDC/HBITMAP/HWND. hwui is
 * Android code: its native window is an ANativeWindow*, as in the Android
 * branch of the Khronos header, and compat/hwui/gl/angle_gl.cc maps it to
 * the Gesher window's HWND. The functions are declared without dllimport:
 * angle_gl.cc defines them in the executable. Same include guard as the
 * Khronos header, which this replaces for hwui only. */
#ifndef __eglplatform_h_
#define __eglplatform_h_

#include <KHR/khrplatform.h>

#ifndef EGLAPI
#define EGLAPI
#endif

#ifndef EGLAPIENTRY
#define EGLAPIENTRY  KHRONOS_APIENTRY
#endif
#define EGLAPIENTRYP EGLAPIENTRY*

struct ANativeWindow;
struct egl_native_pixmap_t;

typedef void*                           EGLNativeDisplayType;
typedef struct egl_native_pixmap_t*     EGLNativePixmapType;
typedef struct ANativeWindow*           EGLNativeWindowType;

/* EGL 1.2 types, renamed for consistency in EGL 1.3 */
typedef EGLNativeDisplayType NativeDisplayType;
typedef EGLNativePixmapType  NativePixmapType;
typedef EGLNativeWindowType  NativeWindowType;

typedef khronos_int32_t EGLint;

/* C++ / C typecast macros for special EGL handle values */
#if defined(__cplusplus)
#define EGL_CAST(type, value) (static_cast<type>(value))
#else
#define EGL_CAST(type, value) ((type) (value))
#endif

#endif /* __eglplatform_h_ */
