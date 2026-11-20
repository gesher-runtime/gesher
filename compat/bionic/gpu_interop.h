// gesher: the process's GPU thread for AHardwareBuffer and ASurfaceControl
// (ADR 0089).
//
// One thread with an OpenGL ES 3 context of its own on Android's default EGL
// display (ANGLE's Direct3D 11 device, egl_gles.cc), current on a 1x1
// pbuffer. Buffers' textures are made and updated there (uploads after CPU
// writes, read-backs before CPU reads) and SurfaceControl trees are composed
// there. ANGLE serializes every context of a display on one Direct3D 11
// device, so work queued here after an app's draw calls sees their results.
// The thread calls ANGLE's Microsoft-ABI entry points (gesher_egl_proc), never
// the System V thunks apps use.
#pragma once

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <functional>

namespace gesher::bionic::gpu {

#define GESHER_GPU_GL_FUNCTIONS(X)                                                              \
  X(glActiveTexture) X(glAttachShader) X(glBindAttribLocation) X(glBindBuffer)                  \
  X(glBindFramebuffer) X(glBindTexture) X(glBindVertexArray) X(glBlendFunc) X(glBufferData)     \
  X(glCheckFramebufferStatus) X(glClear) X(glClearColor) X(glColorMask) X(glCompileShader)       \
  X(glCreateProgram) X(glCreateShader) X(glDeleteBuffers) X(glDeleteFramebuffers)               \
  X(glDeleteShader) X(glDeleteTextures) X(glDisable) X(glDrawArrays) X(glEnable)                \
  X(glEnableVertexAttribArray) X(glFinish) X(glFlush) X(glFramebufferTexture2D)                 \
  X(glGenBuffers) X(glGenFramebuffers) X(glGenTextures) X(glGetError) X(glGetIntegerv)          \
  X(glGetProgramInfoLog) X(glGetProgramiv) X(glGetShaderInfoLog) X(glGetShaderiv)               \
  X(glGetUniformLocation) X(glLinkProgram) X(glPixelStorei) X(glReadPixels) X(glScissor)        \
  X(glShaderSource) X(glTexParameteri) X(glTexStorage2D) X(glTexSubImage2D) X(glUniform1f)      \
  X(glUniform1i) X(glUniform2f) X(glUniform4f) X(glUseProgram) X(glVertexAttribPointer)         \
  X(glViewport)

#define GESHER_GPU_EGL_FUNCTIONS(X)                                                             \
  X(eglBindAPI) X(eglChooseConfig) X(eglClientWaitSyncKHR) X(eglCreateContext)                  \
  X(eglCreateImageKHR) X(eglCreatePbufferSurface) X(eglCreateSyncKHR) X(eglDestroyImageKHR) X(eglDestroySurface)     \
  X(eglDestroySyncKHR) X(eglGetConfigAttrib) X(eglGetCurrentContext) X(eglGetCurrentDisplay)   \
  X(eglGetError) X(eglGetSyncAttribKHR) X(eglMakeCurrent) X(eglQueryContext) X(eglQueryString) \
  X(eglWaitSyncKHR)

// ANGLE's entry points, typed as Microsoft-ABI function pointers.
struct Gl {
#define GESHER_GPU_FIELD(name) decltype(&::name) name = nullptr;
  GESHER_GPU_GL_FUNCTIONS(GESHER_GPU_FIELD)
  GESHER_GPU_EGL_FUNCTIONS(GESHER_GPU_FIELD)
#undef GESHER_GPU_FIELD
};

// ANGLE's functions (resolved once); null when ANGLE is missing or lacks one.
const Gl* Functions();

class Thread {
 public:
  // The process's GPU thread, started on first use; null without ANGLE, a
  // device, or a context (the reason is printed once).
  static Thread* Get();

  EGLDisplay display() const { return display_; }
  EGLContext context() const { return context_; }
  EGLConfig config() const { return config_; }
  const Gl& gl() const { return *gl_; }
  int max_texture_size() const { return max_texture_size_; }
  bool color_buffer_float() const { return color_buffer_float_; }

  // Runs `fn` on the GPU thread and waits for it; inline when called there.
  void Run(const std::function<void()>& fn);
  // Queues `fn` behind what is already queued; runs inline on the thread.
  void Post(std::function<void()> fn);
  bool OnThread() const;

  // The composing program (on the GPU thread).
  struct Program {
    GLuint id = 0;
    GLint viewport = -1, texture = -1, color = -1, alpha = -1, mode = -1, opaque = -1;
    GLuint vbo = 0;
  };
  const Program& program() const { return program_; }

 private:
  Thread() = default;
  bool Start();
  void Main();
  bool InitOnThread();

  struct Queue;
  Queue* queue_ = nullptr;
  const Gl* gl_ = nullptr;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLConfig config_ = nullptr;
  EGLSurface surface_ = EGL_NO_SURFACE;
  int max_texture_size_ = 0;
  bool color_buffer_float_ = false;
  Program program_;
};

}  // namespace gesher::bionic::gpu
