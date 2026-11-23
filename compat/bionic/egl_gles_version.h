// gesher: which Direct3D 11 device Android's default EGL display (egl_gles.cc)
// runs on, and the OpenGL ES version the device declares for it
// (ro.opengles.version, FeatureInfo.reqGlEsVersion; docs/system-features.md).
// Platform-independent, so tests/native/egl_gles_version_test.cc checks the
// policy itself.
//
// OpenGL ES 3.0 only on a hardware Direct3D 11 device (by speed). A software
// device (WARP; the Microsoft Basic Render Driver that a machine without a
// display driver offers as its "hardware" adapter, which is WARP itself; an
// adapter the device query cannot identify) is replaced by WARP opened capped
// at Direct3D feature level 10_0 (ANGLE's EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR/
// MINOR_ANGLE), where ANGLE gives ES 2.0 only: no EGL_OPENGL_ES3_BIT_KHR
// config, ES 3 contexts refused with EGL_BAD_MATCH, GL_VERSION "OpenGL ES 2.0".
// The declared version is read from the configs of the display that is kept,
// and a software display that still offers ES 3.0 is not kept, so what the
// device declares and what EGL gives cannot differ, as on an Android device
// whose GPU has no ES 3.0.
#pragma once

#include <cstdint>

namespace gesher::bionic::gl {

constexpr int kGlEsVersionUndefined = 0;  // FeatureInfo.GL_ES_VERSION_UNDEFINED
constexpr int kGlEsVersion20 = 0x20000;
constexpr int kGlEsVersion30 = 0x30000;

// The feature level a software device is capped at: 10_0, the highest without
// ES 3.0 in ANGLE's Direct3D 11 renderer (ES 3.0 needs 10_1).
constexpr int kSoftwareMaxFeatureLevelMajor = 10;
constexpr int kSoftwareMaxFeatureLevelMinor = 0;

// DXGI_ADAPTER_DESC1: DXGI_ADAPTER_FLAG_SOFTWARE, and the PCI identity of the
// Microsoft Basic Render Driver (WARP's adapter, also when ANGLE asked for the
// hardware device).
constexpr uint32_t kDxgiAdapterFlagSoftware = 2;
constexpr uint32_t kMicrosoftVendorId = 0x1414;
constexpr uint32_t kBasicRenderDriverDeviceId = 0x8c;

// The adapter of the display's Direct3D 11 device, from EGL_EXT_device_query
// (EGL_D3D11_DEVICE_ANGLE) and IDXGIAdapter1::GetDesc1. `known` is false when
// any step failed.
struct AdapterIdentity {
  bool known = false;
  uint32_t flags = 0;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
};

// A GPU: identified, not flagged software, not the Basic Render Driver.
inline bool HardwareAdapter(const AdapterIdentity& adapter) {
  if (!adapter.known) return false;
  if ((adapter.flags & kDxgiAdapterFlagSoftware) != 0) return false;
  return !(adapter.vendor_id == kMicrosoftVendorId &&
           adapter.device_id == kBasicRenderDriverDeviceId);
}

// What a display was found to be.
struct DisplayFacts {
  bool display = false;      // opened and initialized (not EGL_NO_DISPLAY)
  bool warp = false;         // opened as ANGLE's WARP device
  AdapterIdentity adapter;   // its Direct3D 11 device's adapter
  bool es3_config = false;   // an EGL_OPENGL_ES3_BIT_KHR config
  bool es2_config = false;   // an EGL_OPENGL_ES2_BIT config
};

// Whether the display is hardware-accelerated Direct3D 11.
inline bool HardwareDisplay(const DisplayFacts& facts) {
  return facts.display && !facts.warp && HardwareAdapter(facts.adapter);
}

// Whether the hardware device ANGLE opened (uncapped) becomes the default
// display: a GPU with any ES config (its own ES 3.0, else ES 2.0, faster than
// WARP either way). Anything else is replaced by the capped WARP device.
inline bool KeepHardwareDevice(const DisplayFacts& facts) {
  return HardwareDisplay(facts) && (facts.es3_config || facts.es2_config);
}

// Whether a display may be the default one: it has ES configs, and ES 3.0
// configs only on hardware. A software display offering ES 3.0 (the cap not
// in effect) is refused rather than declared lower than what it gives.
inline bool AcceptDisplay(const DisplayFacts& facts) {
  if (!facts.display || !(facts.es3_config || facts.es2_config)) return false;
  return HardwareDisplay(facts) || !facts.es3_config;
}

// The version declared for a display: what its configs give, for an accepted
// display; nothing otherwise.
inline int DeclaredGlEsVersion(const DisplayFacts& facts) {
  if (!AcceptDisplay(facts)) return kGlEsVersionUndefined;
  return facts.es3_config ? kGlEsVersion30 : kGlEsVersion20;
}

}  // namespace gesher::bionic::gl
