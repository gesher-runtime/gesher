// Gesher: what the generated EGL/GLES forwarders (gen_gl_forwarders.py) include.
#pragma once

// Every prototype, so each forwarder is checked against the header's.
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#include <atomic>

// ANGLE's Microsoft-ABI entry point `name`; aborts with a message when ANGLE
// is missing or lacks it (opengl_jni.cc).
void* gesher_gl_forwarder_resolve(const char* name);
