// Gesher: hwui's OpenGL ES over ANGLE (angle_gl.cc; docs/angle.md, ADR 0030).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// Whether the SkiaGL pipeline can run: ANGLE loads, its Direct3D 11 display
// initializes, an RGBA8 ES context on a pbuffer becomes current and Skia gets
// a GrGLInterface. *detail says what was found (GL_RENDERER, GL_VERSION, EGL
// version) or what failed. Leaves no context current.
bool gesher_hwui_gl_usable(std::string* detail);

// The debug read-back (com.gesher.app.graphics.GraphicsDebug.setGlReadback,
// or GESHER_HWUI_GL_READBACK=1): before each eglSwapBuffers of a window
// surface, glReadPixels keeps a copy of the frame for its HWND.
void gesher_hwui_gl_set_readback(bool on);

// The last frame kept for `hwnd`: ARGB colors (0xAARRGGBB), rows top-down.
bool gesher_hwui_gl_last_frame(void* hwnd, std::vector<uint32_t>* argb, int* width, int* height);

// Called by eglSwapBuffersWithDamageKHR before it presents.
void gesher_hwui_gl_before_swap(void* display, void* surface);
