// gesher: AHardwareBuffer for an app's ELF libraries (ADR 0089). See
// hardware_buffer.cc for the NDK entry points; this is what the rest of the
// graphics half (surface_control.cc, egl_native_buffer.cc) uses.
//
// A buffer is shared memory (a memfd, mapped in every process that has it:
// AHardwareBuffer_sendHandleToUnixSocket passes the memfd), described as
// AHardwareBuffer_Desc. Its GPU side is per process: a texture of the
// process's GPU thread (gpu_interop.h) and an EGLImage made from it, which is
// what eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) gives the app; every
// sibling of that image (the app's textures, the compositor's) is the same
// Direct3D 11 storage inside ANGLE. Memory and texture are kept coherent at
// Android's synchronization points: a CPU lock reads the texture back when the
// GPU may have written it, a CPU unlock after writing uploads, a buffer a
// SurfaceControl transaction or an import takes is uploaded when its memory may
// be newer (another process's writes), and sending a buffer reads it back.
#pragma once

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
// The prototypes, for gpu_interop.h's decltype(&::eglXxx), whichever comes first.
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <android/hardware_buffer.h>

#include <stddef.h>
#include <stdint.h>

namespace gesher::bionic::ahb {

struct Buffer;  // what an app's AHardwareBuffer* points to

// The buffer `p` points to, or null when it is not one (its magic checked).
Buffer* FromApp(const void* p);
inline AHardwareBuffer* ToApp(Buffer* b) { return reinterpret_cast<AHardwareBuffer*>(b); }

void Acquire(Buffer* b);
void Release(Buffer* b);
const AHardwareBuffer_Desc& Desc(const Buffer* b);

// --- the GPU side (each runs on the GPU thread; callable from any thread) ----

// For eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID): the EGLImage aliasing the
// buffer's texture on `display`, made (with the buffer's contents) on first
// use. One more app reference to it and to the buffer. EGL_NO_IMAGE_KHR, with
// *egl_error set, when the buffer has no GPU form (format, usage, no device).
EGLImageKHR AcquireAppImage(Buffer* b, EGLDisplay display, EGLint* egl_error);
// For eglDestroyImageKHR: 1 when one app reference to `image` was dropped, 0
// when `image` is not a buffer's (ANGLE's own), -1 when it is a buffer's image
// without app references left (EGL_BAD_PARAMETER).
int ReleaseAppImage(EGLImageKHR image);

// On the GPU thread, for the compositor: the GPU thread's texture holding the
// buffer's current contents (made or uploaded as needed), or 0 when the
// buffer has no GPU form.
GLuint CompositorTexture(Buffer* b);

// A SurfaceControl transaction took the buffer: memory written by another
// process becomes visible to the next composition.
void NoteSubmitted(Buffer* b);

}  // namespace gesher::bionic::ahb
