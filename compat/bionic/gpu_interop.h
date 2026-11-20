// gesher: the process's GPU thread for AHardwareBuffer and ASurfaceControl
// (ADR 0089).
//
// One thread owns a Direct3D 11 device of its own, on the adapter of ANGLE's
// device (Android's default EGL display, egl_gles.cc), and its immediate
// context. Buffers' textures are created there as shared resources and
// opened on ANGLE's device, where the app's EGLImages of them live
// (EGL_ANGLE_image_d3d11_texture); uploads after CPU writes, read-backs before
// CPU reads and SurfaceControl composition run on this device. The thread
// never makes an ANGLE context current: ANGLE's Direct3D 11 back end keeps one
// state cache per device and resets it only in eglMakeCurrent, so a context
// of its own beside the app's contexts would corrupt both. ANGLE's device is
// used only through its free-threaded methods (OpenSharedResource).
#pragma once

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <cstddef>
#include <cstdint>
#include <functional>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gesher::bionic::gpu {

#define GESHER_GPU_EGL_FUNCTIONS(X)                                                             \
  X(eglChooseConfig) X(eglClientWaitSyncKHR) X(eglCreateImageKHR) X(eglCreatePbufferSurface)   \
  X(eglCreateSyncKHR) X(eglDestroyImageKHR) X(eglDestroySurface) X(eglDestroySyncKHR)          \
  X(eglGetConfigAttrib) X(eglGetCurrentContext) X(eglGetCurrentDisplay) X(eglGetError)          \
  X(eglGetSyncAttribKHR) X(eglQueryContext) X(eglQueryString) X(eglWaitSyncKHR)                \
  X(eglQueryDisplayAttribEXT) X(eglQueryDeviceAttribEXT)

// ANGLE's EGL entry points, typed as Microsoft-ABI function pointers.
struct Gl {
#define GESHER_GPU_FIELD(name) decltype(&::name) name = nullptr;
  GESHER_GPU_EGL_FUNCTIONS(GESHER_GPU_FIELD)
#undef GESHER_GPU_FIELD
};

// ANGLE's functions (resolved once); null when ANGLE is missing or lacks one.
const Gl* Functions();

// --- textures and composition (each on the GPU thread) ----------------------------------------

// A texture of the GPU thread's device, shared with ANGLE's device.
struct Texture;
enum class TextureFormat { kRgba8, kRgba16f, kRgb10a2, kR8 };

// A width x height texture holding `data` (rows `row_bytes` apart, in the
// texture's own layout); null on failure.
Texture* CreateTexture(TextureFormat format, int width, int height, const void* data, size_t row_bytes);
// Uploads a width x height block at (x, y); ANGLE's device sees it on return.
void UpdateTexture(Texture* texture, int x, int y, int width, int height, const void* data, size_t row_bytes);
// Reads a width x height block at (x, y) into `out`; false on failure.
bool ReadTexture(Texture* texture, int x, int y, int width, int height, void* out, size_t row_bytes);
// The texture opened on ANGLE's device (an ID3D11Texture2D*), for
// eglCreateImageKHR(EGL_D3D11_TEXTURE_ANGLE).
void* AngleTexture(Texture* texture);
void DestroyTexture(Texture* texture);

// What a composition draws: a texture (or a color) on a quad, clipped.
struct ComposeOp {
  Texture* texture = nullptr;  // null: `color`
  float quad[4][4] = {};       // TL, TR, BL, BR: x, y (target pixels, top-left origin), u, v
  int scissor[4] = {};         // x, y (top), width, height
  bool opaque = false;         // alpha taken as 1
  float alpha = 1;             // plane alpha
  float color[4] = {};         // premultiplied
};

// Composes `ops` over transparent black into a width x height frame kept for
// `target` (any key: a window) and reads it back as RGBA (or BGRA) rows into
// `out`, `out_stride` pixels apart, at most out_width x out_height.
bool Compose(const void* target, int width, int height, const ComposeOp* ops, size_t count, uint8_t* out,
             int out_stride, int out_width, int out_height, bool bgra);
// Drops what Compose keeps for `target`.
void ForgetTarget(const void* target);

class Thread {
 public:
  // The process's GPU thread, started on first use; null without ANGLE's
  // Direct3D 11 device or a device of its own (the reason is printed once).
  static Thread* Get();

  EGLDisplay display() const { return display_; }
  ID3D11Device* device() const { return device_; }           // the thread's own
  ID3D11DeviceContext* context() const { return context_; }  // on the thread only
  ID3D11Device* angle_device() const { return angle_device_; }
  int max_texture_size() const { return max_texture_size_; }
  bool color_buffer_float() const { return color_buffer_float_; }

  // Runs `fn` on the GPU thread and waits for it; inline when called there.
  void Run(const std::function<void()>& fn);
  // Queues `fn` behind what is already queued; runs inline on the thread.
  void Post(std::function<void()> fn);
  bool OnThread() const;

  // Submits what the immediate context has and waits for the GPU to finish
  // it (so ANGLE's device sees an upload). On the GPU thread.
  void Finish();

  // The composing pipeline (on the GPU thread); see gpu_interop.cc.
  struct Pipeline;
  const Pipeline& pipeline() const { return *pipeline_; }

 private:
  Thread() = default;
  bool Start();
  void Main();
  bool InitOnThread();

  struct Queue;
  Queue* queue_ = nullptr;
  const Gl* gl_ = nullptr;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  ID3D11Device* angle_device_ = nullptr;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  Pipeline* pipeline_ = nullptr;
  int max_texture_size_ = 0;
  bool color_buffer_float_ = false;
};

}  // namespace gesher::bionic::gpu
