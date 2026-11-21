// Gesher media: android.graphics.SurfaceTexture's GL consumer, the
// GL_TEXTURE_EXTERNAL_OES texture an app samples (docs/graphics.md
// "SurfaceTexture in OpenGL ES").
//
// AOSP's SurfaceTexture (libs/nativedisplay/surfacetexture: SurfaceTexture and
// its EGLConsumer) binds each buffer updateTexImage latches to the app's
// texture as an EGLImage (glEGLImageTargetTexture2DOES on
// GL_TEXTURE_EXTERNAL_OES). Here the frames of a SurfaceTexture are RGBA
// pixels in memory (its QueueSurface's), so latching one makes a Direct3D 11
// texture of ANGLE's device holding them (CreateTexture2D with the pixels as
// its initial data: a free-threaded device call; ANGLE's device context is not
// touched), an EGLImage of it (EGL_ANGLE_image_d3d11_texture) and binds that to
// the app's texture. The image and the Direct3D texture are held by the app's
// texture only: they go when it is bound to the next frame or deleted. No GL
// object of Gesher's is made in the app's context, so a SurfaceTexture released
// on a thread without that context leaves nothing behind.
//
// The state machine is AOSP's: a SurfaceTexture made with a texture name is
// attached to the context current at its first updateTexImage; one made
// detached, or detached later, has none until attachToGLContext;
// detachFromGLContext deletes the texture (as AOSP's does); updateTexImage
// needs the attached context current (else INVALID_OPERATION, which the JNI
// turns into IllegalStateException); once released (abandoned) every call
// gives NO_INIT. The transform matrix is the identity until a frame is
// latched, then the vertical flip of a buffer with no crop or transform (rows
// top-down, GL's coordinates bottom-up); the time stamp is the latched frame's.
#pragma once

#include <stdint.h>

#include <memory>
#include <mutex>

#include "queue_surface.h"

namespace gesher::media {

class GlTextureConsumer {
 public:
  // Android's status_t values (utils/Errors.h) the JNI maps to exceptions.
  static constexpr int kOk = 0;
  static constexpr int kNoInit = -19;             // -ENODEV
  static constexpr int kInvalidOperation = -38;   // -ENOSYS
  static constexpr int kUnknownError = static_cast<int>(0x80000000u);

  // `detached`: SurfaceTexture(boolean singleBufferMode), no texture yet;
  // otherwise SurfaceTexture(int texName, ...).
  GlTextureConsumer(bool detached, uint32_t tex_name);
  ~GlTextureConsumer();
  GlTextureConsumer(const GlTextureConsumer&) = delete;
  GlTextureConsumer& operator=(const GlTextureConsumer&) = delete;

  // updateTexImage: latches `latest` (the surface's newest frame, or null)
  // into the texture if it is newer than the latched one; binds the texture
  // to GL_TEXTURE_EXTERNAL_OES either way, as AOSP does.
  int UpdateTexImage(const std::shared_ptr<const SurfaceFrame>& latest);
  // releaseTexImage: drops the latched frame; an attached texture shows a
  // 1x1 black image instead (AOSP binds a small debug image).
  int ReleaseTexImage();
  int AttachToContext(uint32_t tex_name);
  int DetachFromContext();
  // SurfaceTexture.release: every later call gives NO_INIT.
  void Abandon();

  int64_t Timestamp() const;
  void TransformMatrix(float out[16]) const;

 private:
  enum class Mode { kAttached, kDetached };

  int CheckEglState(bool context_check);  // mutex_ held
  // Binds `pixels` (width x height RGBA, rows top-down) to tex_name_ through a
  // new image; mutex_ held, display_ set.
  int Bind(const uint32_t* pixels, int width, int height);

  mutable std::mutex mutex_;
  Mode mode_;
  uint32_t tex_name_;
  void* display_ = nullptr;  // EGLDisplay
  void* context_ = nullptr;  // EGLContext
  bool abandoned_ = false;
  std::shared_ptr<const SurfaceFrame> current_;
  uint64_t latched_number_ = 0;
  int64_t timestamp_ns_ = 0;
  float transform_[16];
};

}  // namespace gesher::media
