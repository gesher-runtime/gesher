/*
 * Gesher: native windows for the NDK (docs/graphics.md, "ANativeWindow for
 * libandroid").
 *
 * dalvikvm has hostgraphics' ANativeWindow functions (acquire/release,
 * getWidth/getHeight/getFormat, dequeue/cancel, the interceptors) and
 * libandroid_runtime's ANativeWindow_fromSurface; native_window.cc adds the
 * NDK ones a native app draws with (ANativeWindow_lock, unlockAndPost,
 * setBuffersGeometry, ANativeWindow_toSurface, ...). They are plain C
 * functions with the Microsoft x64 ABI, like the rest of the executable; the
 * ELF loader's libandroid binds Android .so files (System V) to them through
 * its thunks.
 *
 * gesher_window_* make a top-level Win32 window with its ANativeWindow
 * (Win32Surface), for tests and for code that has no Java Surface.
 */
#pragma once

#include <stdint.h>

#include <android/native_window.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A hidden top-level window of width x height client pixels (title in UTF-8)
 * and its ANativeWindow, with one reference the caller releases
 * (ANativeWindow_release); the window is destroyed with its last reference.
 * Null if Windows refuses. */
ANativeWindow* gesher_window_create(const char* title, int32_t width, int32_t height);

/* Shows the window (it starts hidden). */
void gesher_window_show(ANativeWindow* window);

/* The window's HWND, or null when `window` is not a Gesher window. */
void* gesher_window_hwnd(ANativeWindow* window);

#ifdef __cplusplus
}
#endif
