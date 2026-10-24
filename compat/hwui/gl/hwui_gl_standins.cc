// Gesher: what hwui's GL pipeline links against but Gesher's host build does
// not have (docs/angle.md, ADR 0030). None of these is reached: they are the
// device's TextureView layers, AHardwareBuffer surfaces and sync files.
//
//  - DeferredLayerUpdater (TextureView's layer, over an ASurfaceTexture):
//    SkiaOpenGLPipeline::createTextureLayer makes one; nothing on the host
//    calls it (HardwareRenderer's texture layers are device-only JNI).
//  - SkSurfaces::WrapAndroidHardwareBuffer and AHardwareBuffer_acquire/
//    release: SkiaGpuPipeline's HardwareBufferRenderer path, which the host's
//    CanvasContext::setHardwareBuffer never enables.
//  - sync_wait (libsync): EglManager waits on Android fence files only, and
//    there are none (no EGL_ANDROID_native_fence_sync on ANGLE).

#include <errno.h>

#include <android/hardware_buffer.h>
#include <include/android/SkSurfaceAndroid.h>
#include <log/log.h>
#include <sync/sync.h>

#include "DeferredLayerUpdater.h"

namespace android {
namespace uirenderer {

DeferredLayerUpdater::DeferredLayerUpdater(RenderState& renderState)
        : mRenderState(renderState)
        , mBlend(false)
        , mSurfaceTexture(nullptr, nullptr)
        , mTransform(nullptr)
        , mGLContextAttached(false)
        , mUpdateTexImage(false)
        , mLayer(nullptr) {
    LOG_ALWAYS_FATAL("gesher: TextureView layers (DeferredLayerUpdater) are not available");
}

DeferredLayerUpdater::~DeferredLayerUpdater() {}

void DeferredLayerUpdater::onContextDestroyed() {}

}  // namespace uirenderer
}  // namespace android

namespace SkSurfaces {

sk_sp<SkSurface> WrapAndroidHardwareBuffer(GrDirectContext*, AHardwareBuffer*, GrSurfaceOrigin,
                                           sk_sp<SkColorSpace>, const SkSurfaceProps*, bool) {
    return nullptr;
}

}  // namespace SkSurfaces

extern "C" void AHardwareBuffer_acquire(AHardwareBuffer*) {}

extern "C" void AHardwareBuffer_release(AHardwareBuffer*) {}

extern "C" int sync_wait(int, int) {
    errno = EINVAL;
    return -1;
}
