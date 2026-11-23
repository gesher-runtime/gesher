// gesher: the OpenGL ES version the device declares (ro.opengles.version,
// FeatureInfo.reqGlEsVersion; docs/system-features.md), from what Android's
// default EGL display (egl_gles.cc) runs on. Platform-independent, so
// tests/native/egl_gles_version_test.cc checks the policy itself.
//
// ES 3.0 is declared only when ANGLE runs on a hardware Direct3D 11 device that
// gives ES 3.0 configs. On WARP, or on the Microsoft Basic Render Driver that a
// machine without a display driver offers as its "hardware" adapter (WARP
// itself), or on a device whose adapter cannot be identified, the rendering is
// software: the device declares ES 2.0, as an Android device whose GPU does not
// support ES 3.0 does, although WARP can still create ES 3.0 contexts. Without
// a display, nothing (GL_ES_VERSION_UNDEFINED).
#pragma once

#include <cstdint>

namespace gesher::bionic::gl {

constexpr int kGlEsVersionUndefined = 0;  // FeatureInfo.GL_ES_VERSION_UNDEFINED
constexpr int kGlEsVersion20 = 0x20000;
constexpr int kGlEsVersion30 = 0x30000;

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

// What the default display was found to be.
struct DisplayFacts {
  bool display = false;      // a default display exists (not EGL_NO_DISPLAY)
  bool warp = false;         // opened as ANGLE's WARP device
  AdapterIdentity adapter;   // its Direct3D 11 device's adapter
  bool es3_config = false;   // an EGL_OPENGL_ES3_BIT_KHR config
  bool es2_config = false;   // an EGL_OPENGL_ES2_BIT config
};

// Whether the display is hardware-accelerated Direct3D 11.
inline bool HardwareDisplay(const DisplayFacts& facts) {
  return facts.display && !facts.warp && HardwareAdapter(facts.adapter);
}

// The version declared for that display.
inline int DeclaredGlEsVersion(const DisplayFacts& facts) {
  if (!facts.display) return kGlEsVersionUndefined;
  if (HardwareDisplay(facts) && facts.es3_config) return kGlEsVersion30;
  if (facts.es2_config || facts.es3_config) return kGlEsVersion20;
  return kGlEsVersionUndefined;
}

// Whether the hardware device ANGLE opened becomes the default display: a GPU
// with any ES config (its own ES 3.0, else ES 2.0, faster than WARP either
// way), or an unidentified/software "hardware" adapter with ES 3.0 configs (the
// Basic Render Driver, already WARP). Otherwise WARP is tried.
inline bool KeepHardwareDevice(const DisplayFacts& facts) {
  if (!facts.display || facts.warp) return false;
  if (HardwareAdapter(facts.adapter)) return facts.es3_config || facts.es2_config;
  return facts.es3_config;
}

}  // namespace gesher::bionic::gl
