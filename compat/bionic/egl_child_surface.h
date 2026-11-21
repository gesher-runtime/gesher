// gesher (surfaces track, ADR 0056): EGL window surfaces on ANativeWindows
// that are not Gesher windows (egl_child_surface.cc).
//
// A SurfaceView's surface, a SurfaceTexture's or an ImageReader's is an
// ANativeWindow with no HWND of its own: its frames go to a consumer in the
// process (hwui draws them where the view is). ANGLE needs a window for a
// window surface, so such an ANativeWindow gets a hidden helper window of its
// size; before each eglSwapBuffers the frame is posted to ANativeWindow (lock,
// unlockAndPost). ANGLE's reverse-row and BGRA readback extensions permit
// direct readback into compatible destination buffers; other formats use a
// clipped fallback. The GPU-to-CPU transfer remains synchronous, with frames
// posted at most 60 times a second (a high-resolution timer), as a display's
// vsync paces them on a device; the helper's swap chain presents with
// interval 0, since nothing shows it. The helper window follows the
// ANativeWindow's size (setFixedSize, a resized view), which ANGLE picks up at
// the next swap.
#pragma once

#include <EGL/egl.h>

#include <android/native_window.h>

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

namespace gesher::bionic::gl {

// A hidden helper window for `window` (an ANativeWindow without an HWND),
// sized to it; null when `window` is not a valid ANativeWindow.
HWND CreateChildHelper(ANativeWindow* window);
// Undoes CreateChildHelper when ANGLE refused the surface.
void DestroyChildHelper(HWND helper);
// The EGL surface ANGLE made on `helper` draws for `window` (which the
// caller holds a reference to).
void ChildSurfaceCreated(EGLSurface surface, ANativeWindow* window, HWND helper);
// The ANativeWindow a child surface posts to, or null for other surfaces.
ANativeWindow* ChildSurfaceWindow(EGLSurface surface);
// True when `surface` was a child surface; its helper window is gone then.
bool ChildSurfaceDestroyed(EGLSurface surface);
// Called before ANGLE's eglSwapBuffers: for a child surface current on this
// thread, posts its frame to its ANativeWindow. Whether ANGLE's swap is still
// to be made: true for other surfaces, and for a child surface only when its
// size changed (ANGLE resizes the surface in the swap); otherwise the helper's
// swap chain is not presented (nothing shows it) and the swap is complete.
bool ChildSurfaceBeforeSwap(EGLDisplay display, EGLSurface surface);
// eglSwapInterval: with a child surface current on this thread, ANGLE's swap
// chain on its hidden helper window keeps interval 0 (waiting for the
// vertical blank to show a window nobody sees only delayed each frame by up to
// two refreshes; the frames are paced in ChildSurfaceBeforeSwap instead), and
// *result is ANGLE's answer; false for other surfaces (the caller forwards).
bool ChildSurfaceSwapInterval(EGLDisplay display, EGLBoolean* result);

}  // namespace gesher::bionic::gl
