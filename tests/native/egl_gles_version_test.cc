// Portable tests of the default EGL display's device choice and the OpenGL ES
// version the device declares for it (compat/bionic/egl_gles_version.h,
// docs/system-features.md): ES 3.0 only on a hardware Direct3D 11 device; a
// software device is kept only capped at ES 2.0, and the declared version is
// always what the kept display's configs give.
//
// Built with -DGESHER_EGL_VERSION_NEGATIVE_CONTROL, the checks run against the
// policy of commit 28143d85a (2.0 declared on WARP while its ES 3.0 configs
// stay) and must fail (tools/test_hwui_renderer_policy.py expects that).
#include "../../compat/bionic/egl_gles_version.h"

#include <cstdio>
#include <cstdlib>

using namespace gesher::bionic::gl;

namespace {

int failures = 0;

void Check(bool ok, const char* reason) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", reason);
    ++failures;
  }
}

#ifdef GESHER_EGL_VERSION_NEGATIVE_CONTROL
// Test-only: the uncapped policy, ES 3.0 configs kept on software.
bool Accept(const DisplayFacts& f) { return f.display && (f.es3_config || f.es2_config); }
int Declared(const DisplayFacts& f) {
  if (!Accept(f)) return kGlEsVersionUndefined;
  return HardwareDisplay(f) && f.es3_config ? kGlEsVersion30 : kGlEsVersion20;
}
#else
bool Accept(const DisplayFacts& f) { return AcceptDisplay(f); }
int Declared(const DisplayFacts& f) { return DeclaredGlEsVersion(f); }
#endif

AdapterIdentity Adapter(uint32_t vendor, uint32_t device, uint32_t flags = 0) {
  AdapterIdentity a;
  a.known = true;
  a.vendor_id = vendor;
  a.device_id = device;
  a.flags = flags;
  return a;
}

DisplayFacts Display(bool warp, AdapterIdentity adapter, bool es3, bool es2) {
  DisplayFacts f;
  f.display = true;
  f.warp = warp;
  f.adapter = adapter;
  f.es3_config = es3;
  f.es2_config = es2;
  return f;
}

// The version the display's configs give: what a kept display must declare.
int ConfigVersion(const DisplayFacts& f) {
  return f.es3_config ? kGlEsVersion30 : f.es2_config ? kGlEsVersion20 : kGlEsVersionUndefined;
}

}  // namespace

int main() {
  const AdapterIdentity intel = Adapter(0x8086, 0x9a49);
  const AdapterIdentity nvidia = Adapter(0x10de, 0x2882);
  const AdapterIdentity basic_render = Adapter(kMicrosoftVendorId, kBasicRenderDriverDeviceId,
                                               kDxgiAdapterFlagSoftware);
  const AdapterIdentity basic_render_unflagged = Adapter(kMicrosoftVendorId, kBasicRenderDriverDeviceId);
  const AdapterIdentity flagged_software = Adapter(0x1234, 0x5678, kDxgiAdapterFlagSoftware);
  const AdapterIdentity unknown;  // the device query failed

  // Hardware: a GPU at feature level 10_1 or above declares ES 3.0; below, ES 2.0.
  Check(Declared(Display(false, intel, true, true)) == kGlEsVersion30, "hardware GPU with ES 3.0: 3.0");
  Check(Declared(Display(false, nvidia, true, true)) == kGlEsVersion30, "second vendor with ES 3.0: 3.0");
  Check(Declared(Display(false, intel, false, true)) == kGlEsVersion20, "GPU without ES 3.0: 2.0");

  // Capped WARP (no ES 3.0 configs): kept, ES 2.0.
  Check(Accept(Display(true, basic_render, false, true)), "capped WARP is kept");
  Check(Declared(Display(true, basic_render, false, true)) == kGlEsVersion20, "capped WARP: 2.0");
  Check(Declared(Display(true, unknown, false, true)) == kGlEsVersion20, "capped WARP, unidentified: 2.0");

  // A software display still offering ES 3.0 is refused, whatever its adapter says:
  // never 3.0 there, and never 2.0 declared while ES 3.0 configs remain.
  Check(!Accept(Display(true, basic_render, true, true)), "uncapped WARP is refused");
  Check(!Accept(Display(true, intel, true, true)), "WARP device type, hardware-looking adapter: refused");
  Check(!Accept(Display(false, basic_render, true, true)), "Basic Render Driver as hardware, uncapped: refused");
  Check(!Accept(Display(false, basic_render_unflagged, true, true)), "Basic Render Driver by PCI id: refused");
  Check(!Accept(Display(false, flagged_software, true, true)), "adapter flagged software: refused");
  Check(!Accept(Display(false, unknown, true, true)), "unidentified adapter with ES 3.0: refused");
  Check(Declared(Display(true, basic_render, true, true)) == kGlEsVersionUndefined,
        "uncapped WARP declares nothing");

  // No display, or a display without ES configs: no version.
  Check(Declared(DisplayFacts{}) == kGlEsVersionUndefined, "EGL_NO_DISPLAY: undefined");
  DisplayFacts no_display_but_hw = Display(false, intel, true, true);
  no_display_but_hw.display = false;
  Check(Declared(no_display_but_hw) == kGlEsVersionUndefined, "EGL_NO_DISPLAY with stale facts: undefined");
  Check(Declared(Display(false, intel, false, false)) == kGlEsVersionUndefined, "no ES configs: undefined");

  // Which hardware device the default display keeps.
  Check(KeepHardwareDevice(Display(false, intel, true, true)), "GPU with ES 3.0 is kept");
  Check(KeepHardwareDevice(Display(false, intel, false, true)), "GPU with ES 2.0 only is kept, not WARP");
  Check(!KeepHardwareDevice(Display(false, intel, false, false)), "GPU without ES configs: WARP");
  Check(!KeepHardwareDevice(Display(false, basic_render, true, true)),
        "Basic Render Driver as hardware: replaced by capped WARP");
  Check(!KeepHardwareDevice(Display(false, unknown, true, true)), "unidentified adapter: capped WARP");
  Check(!KeepHardwareDevice(DisplayFacts{}), "no hardware device: WARP");
  Check(!KeepHardwareDevice(Display(true, basic_render, false, true)), "a WARP device is not the hardware one");

  // The invariant over every combination: a kept display declares exactly what
  // its configs give, and a software one never ES 3.0.
  const AdapterIdentity adapters[] = {intel, basic_render, basic_render_unflagged, flagged_software, unknown};
  int combinations = 0;
  for (int display = 0; display < 2; ++display) {
    for (int warp = 0; warp < 2; ++warp) {
      for (const AdapterIdentity& adapter : adapters) {
        for (int es3 = 0; es3 < 2; ++es3) {
          for (int es2 = 0; es2 < 2; ++es2) {
            DisplayFacts f = Display(warp != 0, adapter, es3 != 0, es2 != 0);
            f.display = display != 0;
            ++combinations;
            const int declared = Declared(f);
            if (Accept(f)) {
              Check(declared == ConfigVersion(f), "kept display: declared version is its configs'");
              Check(HardwareDisplay(f) || !f.es3_config, "kept software display offers no ES 3.0");
            } else {
              Check(declared == kGlEsVersionUndefined, "refused display: nothing declared");
            }
            Check(declared != kGlEsVersion30 || HardwareDisplay(f), "ES 3.0 declared only on hardware");
          }
        }
      }
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "egl gles version: %d failed\n", failures);
    return 1;
  }
  std::printf("egl gles version policy all passed (%d combinations)\n", combinations);
  return 0;
}
