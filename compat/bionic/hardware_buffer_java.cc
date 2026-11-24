// gesher: AHardwareBuffer_fromHardwareBuffer for the Java HardwareBuffers an
// ImageReader's images give (Image.getHardwareBuffer), as Flutter's raster
// thread imports a platform view's frame: the NDK buffer of the reader snapshot,
// then eglGetNativeClientBufferANDROID / EGL_NATIVE_BUFFER_ANDROID
// (egl_native_buffer.cc) and a texture.
//
// A snapshot (compat/media/surface/reader_surface.cc) is process memory behind
// the Java object. Its NDK buffer is an RGBA8888 AHardwareBuffer made from the
// pixels on first request and kept with the snapshot, so it lives as long as the
// Java HardwareBuffer and, as the NDK says, fromHardwareBuffer takes no reference.
// Java HardwareBuffers of any other origin do not exist in Gesher (HardwareBuffer
// allocation is not supported), and AHardwareBuffer_toHardwareBuffer is not
// provided: the symbol stays undefined for apps that import it.
#include "abi_types.h"
#include "hardware_buffer.h"
#include "jni_bridge.h"

#include "../media/surface/reader_surface.h"

#include <gesher/bionic_shim.h>

namespace gesher::bionic {
namespace {

void* MakeNative(const media::ReaderSnapshot& snapshot) {
  if (snapshot.width <= 0 || snapshot.height <= 0) return nullptr;
  return ahb::ToApp(ahb::AllocateRgba(static_cast<uint32_t>(snapshot.width),
                                      static_cast<uint32_t>(snapshot.height), snapshot.pixels));
}

void ReleaseNative(void* buffer) {
  if (ahb::Buffer* b = ahb::FromApp(buffer)) ahb::Release(b);
}

// The JNIEnv is the loaded library's System V proxy; the snapshot is read
// through the runtime's own.
AHardwareBuffer* BIONIC_SYSV FromHardwareBuffer(JNIEnv* env, jobject hardware_buffer) {
  if (env == nullptr || hardware_buffer == nullptr) return nullptr;
  return static_cast<AHardwareBuffer*>(media::ReaderSnapshotNativeBuffer(
      RealJniEnv(env), hardware_buffer, MakeNative, ReleaseNative));
}

[[maybe_unused]] const bool g_registered = [] {
  AddBionicSymbol("AHardwareBuffer_fromHardwareBuffer", reinterpret_cast<void*>(&FromHardwareBuffer));
  return true;
}();

}  // namespace
}  // namespace gesher::bionic
