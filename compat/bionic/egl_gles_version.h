// gesher: which Direct3D 11 device Android's default EGL display (egl_gles.cc)
// runs on, and the OpenGL ES version it gives, which is the version the device
// declares (ro.opengles.version, FeatureInfo.reqGlEsVersion;
// docs/system-features.md). Platform-independent, so
// tests/native/egl_gles_version_test.cc checks the policy itself.
//
// The system server probes once per boot, with nothing declared yet: OpenGL ES
// 3.0 only on a hardware Direct3D 11 device that gives it (by speed). A
// software device (WARP; the Microsoft Basic Render Driver that a machine
// without a display driver offers as its "hardware" adapter, which is WARP
// itself; an adapter the device query cannot identify) is replaced by WARP
// opened capped at Direct3D feature level 10_0 (ANGLE's
// EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR/MINOR_ANGLE), where ANGLE gives ES 2.0
// only: no EGL_OPENGL_ES3_BIT_KHR config, ES 3 contexts refused with
// EGL_BAD_MATCH, GL_VERSION "OpenGL ES 2.0". It sets ro.opengles.version to
// what that display gives.
//
// Every process after that (apps) chooses its display to give exactly the
// boot's declaration, whatever the adapter has become meanwhile (a remote
// desktop session on the Basic Render Driver, a driver update or reset,
// GESHER_EGL_DEVICE=warp for one process): declared 3.0, the GPU if it gives
// ES 3.0, else WARP uncapped (which gives ES 3.0, in software); declared 2.0,
// the GPU or WARP capped at feature level 10_0. A display that would give
// another version is not kept.
//
// ES 3.1 and 3.2 are never declared (ANGLE's Direct3D 11 backend marks 3.1
// unfinished; docs/system-features.md), so on a display giving 3.0 an ES 3.1+
// context is refused with EGL_BAD_MATCH, as a 3.0 driver refuses it
// (ContextVersionAllowed).
#pragma once

#include <cstdint>

namespace gesher::bionic::gl {

constexpr int kGlEsVersionUndefined = 0;  // FeatureInfo.GL_ES_VERSION_UNDEFINED
constexpr int kGlEsVersion20 = 0x20000;
constexpr int kGlEsVersion30 = 0x30000;

// The feature level a device is capped at to give ES 2.0 only: 10_0, the
// highest without ES 3.0 in ANGLE's Direct3D 11 renderer (ES 3.0 needs 10_1).
constexpr int kEs2MaxFeatureLevelMajor = 10;
constexpr int kEs2MaxFeatureLevelMinor = 0;

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

// The version a display gives: what its configs offer.
inline int ConfigVersion(const DisplayFacts& facts) {
  if (!facts.display) return kGlEsVersionUndefined;
  return facts.es3_config ? kGlEsVersion30 : facts.es2_config ? kGlEsVersion20 : kGlEsVersionUndefined;
}

// The boot's declaration as the policy uses it: 0x30000, 0x20000, or
// undefined (none yet: the system server's probe, or a process outside a
// Gesher boot). Any other value (a preset build.prop line such as 0x30001)
// counts as undefined: the display is chosen as the probe chooses it, and
// PackageManagerService likewise ignores such a preset and declares the probed
// version (servedDeclaration).
inline int Declaration(int ro_opengles_version) {
  return ro_opengles_version == kGlEsVersion30 || ro_opengles_version == kGlEsVersion20
             ? ro_opengles_version : kGlEsVersionUndefined;
}

// How ANGLE's device is opened for one attempt.
struct Attempt {
  bool warp = false;    // EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE, else hardware
  bool capped = false;  // at most feature level 10_0 (ES 2.0)
};

// The two attempts for a declaration, in order: the hardware device (skipped
// with GESHER_EGL_DEVICE=warp), then WARP. Capped exactly when ES 3.0 must not
// be given: declared 2.0, or WARP without a declaration.
inline Attempt HardwareAttempt(int declared) {
  return Attempt{false, Declaration(declared) == kGlEsVersion20};
}
inline Attempt WarpAttempt(int declared) {
  return Attempt{true, Declaration(declared) != kGlEsVersion30};
}

// Whether a display may be the default one for a declaration. It must have ES
// configs. With a declaration it must give exactly that version; with none
// (the probe), ES 3.0 only on hardware.
inline bool AcceptDisplay(const DisplayFacts& facts, int declared) {
  const int gives = ConfigVersion(facts);
  if (gives == kGlEsVersionUndefined) return false;
  const int want = Declaration(declared);
  if (want != kGlEsVersionUndefined) return gives == want;
  return HardwareDisplay(facts) || gives != kGlEsVersion30;
}

// Whether the hardware device ANGLE opened becomes the default display: a GPU
// (never the software "hardware" adapter) that is acceptable. Without a
// declaration that is any GPU with ES configs, ES 3.0 or 2.0 (faster than
// WARP either way); with one, a GPU giving exactly it.
inline bool KeepHardwareDevice(const DisplayFacts& facts, int declared) {
  return HardwareDisplay(facts) && AcceptDisplay(facts, declared);
}

// The version given and declared for a display: its configs', for an
// accepted display; nothing otherwise.
inline int DeclaredGlEsVersion(const DisplayFacts& facts, int declared) {
  return AcceptDisplay(facts, declared) ? ConfigVersion(facts) : kGlEsVersionUndefined;
}

// Whether a context of `major`.`minor` may be created on the default display
// that gives `version`: up to 3.0 on an ES 3.0 display (no 3.1/3.2), 2.0 on an
// ES 2.0 one (ANGLE itself refuses more there). ES 1.x always (ANGLE emulates
// it on both).
inline bool ContextVersionAllowed(int version, int major, int minor) {
  if (major <= 1) return true;
  if (major == 2) return minor == 0;
  if (major == 3) return version == kGlEsVersion30 && minor == 0;
  return false;
}

}  // namespace gesher::bionic::gl
