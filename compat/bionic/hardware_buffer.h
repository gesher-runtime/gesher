// gesher: AHardwareBuffer for an app's ELF libraries (ADR 0089). See
// hardware_buffer.cc for the NDK entry points; this is what the rest of the
// graphics half (surface_control.cc, egl_native_buffer.cc) uses.
//
// A buffer is shared memory (a memfd, mapped in every process that has it:
// AHardwareBuffer_sendHandleToUnixSocket passes the memfd), described as
// AHardwareBuffer_Desc. Its GPU side is per process: a Direct3D 11 texture of
// the process's GPU thread (gpu_interop.h), shared with ANGLE's device, where
// each eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) makes an EGLImage of it
// (EGL_ANGLE_image_d3d11_texture); the app's textures of those images and the
// compositor sample one storage. Memory and texture are kept coherent at
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

namespace gesher::bionic::gpu {
struct Texture;
}

namespace gesher::bionic::ahb {

struct Buffer;  // what an app's AHardwareBuffer* points to

// The buffer `p` points to, or null when it is not one (its magic checked).
Buffer* FromApp(const void* p);
inline AHardwareBuffer* ToApp(Buffer* b) { return reinterpret_cast<AHardwareBuffer*>(b); }

void Acquire(Buffer* b);
void Release(Buffer* b);

// eglGetNativeClientBufferANDROID's EGLClientBuffer for a buffer (its
// ANativeWindowBuffer, as on Android), and the buffer of one (null if none).
void* ClientBuffer(Buffer* b);
Buffer* FromClientBuffer(const void* client_buffer);
const AHardwareBuffer_Desc& Desc(const Buffer* b);

// --- the GPU side (each runs on the GPU thread; callable from any thread) ----

// For eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID): a new EGLImage of the
// buffer's texture on `display` (the texture made, with the buffer's
// contents, on first use), holding a reference to the buffer.
// EGL_NO_IMAGE_KHR, with *egl_error set, when the buffer has no GPU form
// (format, usage, no device).
EGLImageKHR AcquireAppImage(Buffer* b, EGLDisplay display, EGLint* egl_error);
// For eglDestroyImageKHR: 1 when `image` was one AcquireAppImage made (it is
// destroyed and its reference to the buffer dropped), 0 when it is not.
int ReleaseAppImage(EGLImageKHR image);

// On the GPU thread, for the compositor: the GPU thread's texture holding the
// buffer's current contents (made or uploaded as needed), or null when the
// buffer has no GPU form. *opaque_format: the format has no alpha.
gpu::Texture* CompositorTexture(Buffer* b, bool* opaque_format);

// A SurfaceControl transaction took the buffer: memory written by another
// process becomes visible to the next composition.
void NoteSubmitted(Buffer* b);

}  // namespace gesher::bionic::ahb
