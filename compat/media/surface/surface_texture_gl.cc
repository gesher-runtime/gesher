// Gesher media: SurfaceTexture's GL consumer. See surface_texture_gl.h.
#include "surface_texture_gl.h"

#include <gesher/egl_angle.h>
#include <gesher/perf_trace.h>
#include <log/log.h>

#include <cstring>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <d3d11.h>

namespace gesher::media {
namespace {

// EGL and GLES values (Khronos headers; ANGLE's extensions).
constexpr int32_t kEglNone = 0x3038;
constexpr int32_t kEglExtensions = 0x3055;
constexpr int32_t kEglDeviceExt = 0x322C;
constexpr int32_t kEglD3D11DeviceAngle = 0x33A1;
constexpr uint32_t kEglD3D11TextureAngle = 0x3484;
constexpr uint32_t kGlTextureExternalOes = 0x8D65;
constexpr uint32_t kGlNoError = 0;

// ANGLE's entry points (Microsoft x64 ABI, as this code), resolved once.
struct Angle {
  void* (*eglGetCurrentDisplay)() = nullptr;
  void* (*eglGetCurrentContext)() = nullptr;
  int32_t (*eglGetError)() = nullptr;
  const char* (*eglQueryString)(void*, int32_t) = nullptr;
  void* (*eglCreateImageKHR)(void*, void*, uint32_t, void*, const int32_t*) = nullptr;
  unsigned (*eglDestroyImageKHR)(void*, void*) = nullptr;
  unsigned (*eglQueryDisplayAttribEXT)(void*, int32_t, intptr_t*) = nullptr;
  unsigned (*eglQueryDeviceAttribEXT)(void*, int32_t, intptr_t*) = nullptr;
  void (*glBindTexture)(uint32_t, uint32_t) = nullptr;
  void (*glDeleteTextures)(int32_t, const uint32_t*) = nullptr;
  uint32_t (*glGetError)() = nullptr;
  void (*glEGLImageTargetTexture2DOES)(uint32_t, void*) = nullptr;
};

const Angle* GetAngle() {
  static const Angle* angle = []() -> const Angle* {
    auto* a = new Angle();
    bool ok = true;
    auto resolve = [&ok](auto& slot, const char* name) {
      slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(gesher_egl_proc(name));
      if (slot == nullptr) {
        ALOGE("SurfaceTexture: ANGLE does not provide %s: no GL consumer", name);
        ok = false;
      }
    };
    resolve(a->eglGetCurrentDisplay, "eglGetCurrentDisplay");
    resolve(a->eglGetCurrentContext, "eglGetCurrentContext");
    resolve(a->eglGetError, "eglGetError");
    resolve(a->eglQueryString, "eglQueryString");
    resolve(a->eglCreateImageKHR, "eglCreateImageKHR");
    resolve(a->eglDestroyImageKHR, "eglDestroyImageKHR");
    resolve(a->eglQueryDisplayAttribEXT, "eglQueryDisplayAttribEXT");
    resolve(a->eglQueryDeviceAttribEXT, "eglQueryDeviceAttribEXT");
    resolve(a->glBindTexture, "glBindTexture");
    resolve(a->glDeleteTextures, "glDeleteTextures");
    resolve(a->glGetError, "glGetError");
    resolve(a->glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    if (!ok) {
      delete a;
      return nullptr;
    }
    return a;
  }();
  return angle;
}

// What the current thread's EGL has current (ANGLE's handles, as the app's).
void CurrentEgl(void** display, void** context) {
  const Angle* a = GetAngle();
  *display = a != nullptr ? a->eglGetCurrentDisplay() : nullptr;
  *context = a != nullptr ? a->eglGetCurrentContext() : nullptr;
}

// ANGLE's Direct3D 11 device of `display` (EGL_EXT_device_query), if images of
// its textures can be made (EGL_ANGLE_image_d3d11_texture); null otherwise.
ID3D11Device* DeviceOf(const Angle* a, void* display) {
  static std::mutex mu;
  static void* cached_display = nullptr;
  static ID3D11Device* cached = nullptr;
  std::lock_guard<std::mutex> lock(mu);
  if (display == cached_display) return cached;
  ID3D11Device* device = nullptr;
  const char* extensions = a->eglQueryString(display, kEglExtensions);
  intptr_t egl_device = 0, d3d = 0;
  if (extensions != nullptr && std::strstr(extensions, "EGL_ANGLE_image_d3d11_texture") != nullptr &&
      a->eglQueryDisplayAttribEXT(display, kEglDeviceExt, &egl_device) && egl_device != 0 &&
      a->eglQueryDeviceAttribEXT(reinterpret_cast<void*>(egl_device), kEglD3D11DeviceAngle, &d3d) &&
      d3d != 0) {
    device = reinterpret_cast<ID3D11Device*>(d3d);
  } else {
    ALOGE("SurfaceTexture: display %p has no Direct3D 11 texture images "
          "(EGL_ANGLE_image_d3d11_texture): frames cannot be latched into a texture", display);
  }
  cached_display = display;
  cached = device;
  return device;
}

// AOSP's transform for a buffer without crop or transform: GL samples rows
// bottom-up, buffers hold them top-down (column-major, as GL takes it).
constexpr float kFlipV[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

}  // namespace

GlTextureConsumer::GlTextureConsumer(bool detached, uint32_t tex_name)
    : mode_(detached ? Mode::kDetached : Mode::kAttached), tex_name_(detached ? 0 : tex_name) {
  std::memcpy(transform_, kIdentity, sizeof(transform_));
}

GlTextureConsumer::~GlTextureConsumer() = default;

// EGLConsumer::checkAndUpdateEglStateLocked: the attached display and context
// must be current; the first call after construction (or detach) adopts them
// unless `context_check`.
int GlTextureConsumer::CheckEglState(bool context_check) {
  void* display = nullptr;
  void* context = nullptr;
  CurrentEgl(&display, &context);
  if (!context_check) {
    if (display_ == nullptr) display_ = display;
    if (context_ == nullptr) context_ = context;
  }
  if (display_ != display || display == nullptr) {
    ALOGE("SurfaceTexture: checkAndUpdateEglState: invalid current EGLDisplay");
    return kInvalidOperation;
  }
  if (context_ != context || context == nullptr) {
    ALOGE("SurfaceTexture: checkAndUpdateEglState: invalid current EGLContext");
    return kInvalidOperation;
  }
  return kOk;
}

int GlTextureConsumer::Bind(const uint32_t* pixels, int width, int height) {
  const Angle* a = GetAngle();
  ID3D11Device* device = a != nullptr ? DeviceOf(a, display_) : nullptr;
  if (device == nullptr) return kUnknownError;
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial = {};
  initial.pSysMem = pixels;
  initial.SysMemPitch = static_cast<UINT>(width) * 4;
  ID3D11Texture2D* texture = nullptr;
  HRESULT hr = device->CreateTexture2D(&desc, &initial, &texture);
  if (FAILED(hr) || texture == nullptr) {
    ALOGE("SurfaceTexture: no %dx%d texture for the frame (0x%08lx)", width, height,
          static_cast<unsigned long>(hr));
    return kUnknownError;
  }
  const int32_t attribs[] = {kEglNone};
  void* image = a->eglCreateImageKHR(display_, nullptr, kEglD3D11TextureAngle, texture, attribs);
  texture->Release();  // the image holds it
  if (image == nullptr) {
    ALOGE("SurfaceTexture: no EGLImage of the %dx%d frame (0x%x)", width, height, a->eglGetError());
    return kUnknownError;
  }
  // EGLConsumer::bindTextureImageLocked clears earlier GL errors, binds the
  // texture, targets the image and checks that it took.
  for (int i = 0; i < 16 && a->glGetError() != kGlNoError; i++) {
  }
  a->glBindTexture(kGlTextureExternalOes, tex_name_);
  a->glEGLImageTargetTexture2DOES(kGlTextureExternalOes, image);
  const uint32_t error = a->glGetError();
  // The texture holds the image (and the image the Direct3D texture) from
  // now on, until it is bound to another image or deleted.
  a->eglDestroyImageKHR(display_, image);
  if (error != kGlNoError) {
    ALOGE("SurfaceTexture: bindTextureImage: error binding external image: 0x%04x", error);
    return kUnknownError;
  }
  return kOk;
}

int GlTextureConsumer::UpdateTexImage(const std::shared_ptr<const SurfaceFrame>& latest) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (abandoned_) {
    ALOGE("SurfaceTexture: updateTexImage: SurfaceTexture is abandoned!");
    return kNoInit;
  }
  if (int err = CheckEglState(false); err != kOk) return err;
  const Angle* a = GetAngle();
  if (latest == nullptr || latest->number == latched_number_ || latest->width <= 0 ||
      latest->height <= 0) {
    // No new buffer: the texture is bound regardless.
    a->glBindTexture(kGlTextureExternalOes, tex_name_);
    return kOk;
  }
  // Frames the producer queued since the last latch that were never latched.
  static gesher::perf::Site site("SurfaceTexture: latch a frame (count: frames skipped)");
  const int64_t start = gesher::perf::Now();
  if (int err = Bind(latest->pixels.data(), latest->width, latest->height); err != kOk) {
    a->glBindTexture(kGlTextureExternalOes, tex_name_);
    return err;
  }
  site.Add(gesher::perf::Now() - start,
           latched_number_ != 0 && latest->number > latched_number_ + 1
               ? static_cast<int64_t>(latest->number - latched_number_ - 1) : 0);
  current_ = latest;
  latched_number_ = latest->number;
  timestamp_ns_ = latest->timestamp_ns;
  std::memcpy(transform_, kFlipV, sizeof(transform_));
  return kOk;
}

int GlTextureConsumer::ReleaseTexImage() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (abandoned_) {
    ALOGE("SurfaceTexture: releaseTexImage: SurfaceTexture is abandoned!");
    return kNoInit;
  }
  if (mode_ == Mode::kAttached) {
    if (int err = CheckEglState(true); err != kOk) return err;
  }
  if (current_ == nullptr) return kOk;
  current_.reset();
  timestamp_ns_ = 0;
  if (mode_ == Mode::kAttached) {
    static const uint32_t kBlack = 0xff000000u;
    return Bind(&kBlack, 1, 1);
  }
  return kOk;
}

int GlTextureConsumer::AttachToContext(uint32_t tex_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (abandoned_) {
    ALOGE("SurfaceTexture: attachToContext: abandoned SurfaceTexture");
    return kNoInit;
  }
  if (mode_ != Mode::kDetached) {
    ALOGE("SurfaceTexture: attachToContext: SurfaceTexture is already attached to a context");
    return kInvalidOperation;
  }
  void* display = nullptr;
  void* context = nullptr;
  CurrentEgl(&display, &context);
  if (display == nullptr) {
    ALOGE("SurfaceTexture: attachToContext: invalid current EGLDisplay");
    return kInvalidOperation;
  }
  if (context == nullptr) {
    ALOGE("SurfaceTexture: attachToContext: invalid current EGLContext");
    return kInvalidOperation;
  }
  const Angle* a = GetAngle();
  // The texture is bound whether or not there is a frame.
  a->glBindTexture(kGlTextureExternalOes, tex_name);
  display_ = display;
  context_ = context;
  tex_name_ = tex_name;
  mode_ = Mode::kAttached;
  // The latched frame goes on in the new texture.
  if (current_ != nullptr) return Bind(current_->pixels.data(), current_->width, current_->height);
  return kOk;
}

int GlTextureConsumer::DetachFromContext() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (abandoned_) {
    ALOGE("SurfaceTexture: detachFromContext: abandoned SurfaceTexture");
    return kNoInit;
  }
  if (mode_ != Mode::kAttached) {
    ALOGE("SurfaceTexture: detachFromContext: SurfaceTexture is not attached to a GL context");
    return kInvalidOperation;
  }
  void* display = nullptr;
  void* context = nullptr;
  CurrentEgl(&display, &context);
  if (display_ != display && display_ != nullptr) {
    ALOGE("SurfaceTexture: detachFromContext: invalid current EGLDisplay");
    return kInvalidOperation;
  }
  if (context_ != context && context_ != nullptr) {
    ALOGE("SurfaceTexture: detachFromContext: invalid current EGLContext");
    return kInvalidOperation;
  }
  // AOSP deletes the texture (and so the image bound to it) with its context.
  if (display != nullptr && context != nullptr) {
    const uint32_t name = tex_name_;
    GetAngle()->glDeleteTextures(1, &name);
  }
  display_ = nullptr;
  context_ = nullptr;
  mode_ = Mode::kDetached;
  return kOk;
}

void GlTextureConsumer::Abandon() {
  std::lock_guard<std::mutex> lock(mutex_);
  abandoned_ = true;
  current_.reset();
}

int64_t GlTextureConsumer::Timestamp() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return timestamp_ns_;
}

void GlTextureConsumer::TransformMatrix(float out[16]) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::memcpy(out, transform_, sizeof(transform_));
}

}  // namespace gesher::media
