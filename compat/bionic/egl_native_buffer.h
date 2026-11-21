// gesher: Android's EGL for native buffers and fences (ADR 0089); see
// egl_native_buffer.cc.
#pragma once

namespace gesher::bionic::gl {

// Adds the System V entry points of egl_native_buffer.cc through `add` (which
// puts them in the bionic shim's table and eglGetProcAddress's): called by
// egl_gles.cc's registration after its own, so these replace the plain
// ANGLE thunks of the same names.
void RegisterNativeBufferEgl(void (*add)(const char* name, void* fn));

}  // namespace gesher::bionic::gl
