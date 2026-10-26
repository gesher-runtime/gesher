// gesher (surfaces track, ADR 0056): EGL window surfaces on ANativeWindows
// that are not Gesher windows (egl_child_surface.cc).
//
// A SurfaceView's surface, a SurfaceTexture's or an ImageReader's is an
// ANativeWindow with no HWND of its own: its frames go to a consumer in the
// process (hwui draws them where the view is). ANGLE needs a window for a
// window surface, so such an ANativeWindow gets a hidden helper window of its
// size; before each eglSwapBuffers the frame is read back (glReadPixels of
// the default framebuffer) and posted to the ANativeWindow (lock,
// unlockAndPost), at most 60 frames a second, as a display's vsync paces
// them on a device. The helper window follows the ANativeWindow's size
// (setFixedSize, a resized view), which ANGLE picks up at the next swap.
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
// thread, posts its frame to its ANativeWindow.
void ChildSurfaceBeforeSwap(EGLDisplay display, EGLSurface surface);

}  // namespace gesher::bionic::gl
