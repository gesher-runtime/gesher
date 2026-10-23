// gesher: libandroid.so's ANativeWindow functions for an app's ELF libraries
// (docs/native-libs.md, "Graphics"). The functions themselves are dalvikvm's
// Microsoft-ABI ones (compat/hwui/window/gesher_native_window.h:
// hostgraphics' acquire/release/getWidth/getHeight/getFormat/
// tryAllocateBuffers and Gesher's lock/unlockAndPost/setBuffersGeometry/...,
// on any hostgraphics Surface, a Win32Surface in particular); these are the
// System V entries the .so's imports bind to, added to the bionic shim's
// table by a static initializer. ANativeWindow_Buffer and ARect have the same
// layout on both sides (int32_t fields and a pointer).
//
// ANativeWindow_fromSurface/toSurface take a JNIEnv, so they are in
// libandroid.cc with the JNI bridge; ANativeWindow_clearFrameRate is inline in
// the NDK's header.

#include "abi_types.h"

#include <gesher/bionic_shim.h>

#include <android/native_window.h>

#include "gesher_native_window.h"

namespace gesher::bionic {
namespace {

void BIONIC_SYSV Acquire(ANativeWindow* window) { ANativeWindow_acquire(window); }
void BIONIC_SYSV Release(ANativeWindow* window) { ANativeWindow_release(window); }
int32_t BIONIC_SYSV GetWidth(ANativeWindow* window) { return ANativeWindow_getWidth(window); }
int32_t BIONIC_SYSV GetHeight(ANativeWindow* window) { return ANativeWindow_getHeight(window); }
int32_t BIONIC_SYSV GetFormat(ANativeWindow* window) { return ANativeWindow_getFormat(window); }
int32_t BIONIC_SYSV SetBuffersGeometry(ANativeWindow* window, int32_t width, int32_t height,
                                       int32_t format) {
  return ANativeWindow_setBuffersGeometry(window, width, height, format);
}
int32_t BIONIC_SYSV Lock(ANativeWindow* window, ANativeWindow_Buffer* out_buffer,
                         ARect* in_out_dirty_bounds) {
  return ANativeWindow_lock(window, out_buffer, in_out_dirty_bounds);
}
int32_t BIONIC_SYSV UnlockAndPost(ANativeWindow* window) {
  return ANativeWindow_unlockAndPost(window);
}
int32_t BIONIC_SYSV SetBuffersTransform(ANativeWindow* window, int32_t transform) {
  return ANativeWindow_setBuffersTransform(window, transform);
}
int32_t BIONIC_SYSV SetBuffersDataSpace(ANativeWindow* window, int32_t data_space) {
  return ANativeWindow_setBuffersDataSpace(window, data_space);
}
int32_t BIONIC_SYSV GetBuffersDataSpace(ANativeWindow* window) {
  return ANativeWindow_getBuffersDataSpace(window);
}
int32_t BIONIC_SYSV GetBuffersDefaultDataSpace(ANativeWindow* window) {
  return ANativeWindow_getBuffersDefaultDataSpace(window);
}
int32_t BIONIC_SYSV SetFrameRate(ANativeWindow* window, float frame_rate, int8_t compatibility) {
  return ANativeWindow_setFrameRate(window, frame_rate, compatibility);
}
int32_t BIONIC_SYSV SetFrameRateWithChangeStrategy(ANativeWindow* window, float frame_rate,
                                                   int8_t compatibility, int8_t strategy) {
  return ANativeWindow_setFrameRateWithChangeStrategy(window, frame_rate, compatibility, strategy);
}
void BIONIC_SYSV TryAllocateBuffers(ANativeWindow* window) {
  ANativeWindow_tryAllocateBuffers(window);
}

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
  static const Entry kEntries[] = {
      E("ANativeWindow_acquire", Acquire),
      E("ANativeWindow_release", Release),
      E("ANativeWindow_getWidth", GetWidth),
      E("ANativeWindow_getHeight", GetHeight),
      E("ANativeWindow_getFormat", GetFormat),
      E("ANativeWindow_setBuffersGeometry", SetBuffersGeometry),
      E("ANativeWindow_lock", Lock),
      E("ANativeWindow_unlockAndPost", UnlockAndPost),
      E("ANativeWindow_setBuffersTransform", SetBuffersTransform),
      E("ANativeWindow_setBuffersDataSpace", SetBuffersDataSpace),
      E("ANativeWindow_getBuffersDataSpace", GetBuffersDataSpace),
      E("ANativeWindow_getBuffersDefaultDataSpace", GetBuffersDefaultDataSpace),
      E("ANativeWindow_setFrameRate", SetFrameRate),
      E("ANativeWindow_setFrameRateWithChangeStrategy", SetFrameRateWithChangeStrategy),
      E("ANativeWindow_tryAllocateBuffers", TryAllocateBuffers),
  };
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::bionic
