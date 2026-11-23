// Portable tests of the OpenGL ES version the device declares for its default
// EGL display (compat/bionic/egl_gles_version.h, docs/system-features.md):
// ES 3.0 only on a hardware Direct3D 11 device, never on WARP.
//
// Built with -DGESHER_EGL_VERSION_NEGATIVE_CONTROL, the checks run against a
// policy that declares ES 3.0 whenever ES 3.0 configs exist (what Gesher did
// before), and must fail (tools/test_hwui_renderer_policy.py expects that).
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

int Declared(const DisplayFacts& facts) {
#ifdef GESHER_EGL_VERSION_NEGATIVE_CONTROL
  if (!facts.display) return kGlEsVersionUndefined;
  return facts.es3_config ? kGlEsVersion30 : facts.es2_config ? kGlEsVersion20 : 0;
#else
  return DeclaredGlEsVersion(facts);
#endif
}

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

}  // namespace

int main() {
  const AdapterIdentity intel = Adapter(0x8086, 0x9a49);
  const AdapterIdentity nvidia = Adapter(0x10de, 0x2882);
  const AdapterIdentity basic_render = Adapter(kMicrosoftVendorId, kBasicRenderDriverDeviceId,
                                               kDxgiAdapterFlagSoftware);
  // The Basic Render Driver identified by PCI id alone, and any adapter flagged software.
  const AdapterIdentity basic_render_unflagged = Adapter(kMicrosoftVendorId, kBasicRenderDriverDeviceId);
  const AdapterIdentity flagged_software = Adapter(0x1234, 0x5678, kDxgiAdapterFlagSoftware);
  const AdapterIdentity unknown;  // the device query failed

  // Hardware: a GPU at feature level 10_1 or above declares ES 3.0.
  Check(Declared(Display(false, intel, true, true)) == kGlEsVersion30, "hardware GPU with ES 3.0: 3.0");
  Check(Declared(Display(false, nvidia, true, true)) == kGlEsVersion30, "second vendor with ES 3.0: 3.0");
  // A GPU below 10_1 gives ES 2.0 and declares it.
  Check(Declared(Display(false, intel, false, true)) == kGlEsVersion20, "GPU without ES 3.0: 2.0");

  // WARP: never ES 3.0, although it has ES 3.0 configs; ES 2.0 is served.
  Check(Declared(Display(true, basic_render, true, true)) == kGlEsVersion20, "WARP: 2.0, not 3.0");
  // The flag is what counts on WARP, whatever the adapter query says.
  Check(Declared(Display(true, intel, true, true)) == kGlEsVersion20, "WARP device type: 2.0");
  Check(Declared(Display(true, unknown, true, true)) == kGlEsVersion20, "WARP, unidentified: 2.0");
  // "Hardware" device on the Basic Render Driver (a machine without a GPU driver, CI runners).
  Check(Declared(Display(false, basic_render, true, true)) == kGlEsVersion20,
        "hardware request on the Basic Render Driver: 2.0");
  Check(Declared(Display(false, basic_render_unflagged, true, true)) == kGlEsVersion20,
        "Basic Render Driver by PCI id: 2.0");
  Check(Declared(Display(false, flagged_software, true, true)) == kGlEsVersion20,
        "adapter flagged software: 2.0");
  Check(Declared(Display(false, unknown, true, true)) == kGlEsVersion20,
        "unidentified adapter: not proven hardware, 2.0");
  // ES 2.0 from ES 3.0 configs alone (ES 3.0 contexts run ES 2.0 programs).
  Check(Declared(Display(true, basic_render, true, false)) == kGlEsVersion20, "WARP, ES 3 configs only: 2.0");

  // No display, or a display without ES configs: no version.
  Check(Declared(DisplayFacts{}) == kGlEsVersionUndefined, "EGL_NO_DISPLAY: undefined");
  DisplayFacts no_display_but_hw = Display(false, intel, true, true);
  no_display_but_hw.display = false;
  Check(Declared(no_display_but_hw) == kGlEsVersionUndefined, "EGL_NO_DISPLAY with stale facts: undefined");
  Check(Declared(Display(false, intel, false, false)) == kGlEsVersionUndefined, "no ES configs: undefined");

  // Which device the default display keeps (the same facts).
  Check(KeepHardwareDevice(Display(false, intel, true, true)), "GPU with ES 3.0 is kept");
  Check(KeepHardwareDevice(Display(false, intel, false, true)), "GPU with ES 2.0 only is kept, not WARP");
  Check(!KeepHardwareDevice(Display(false, intel, false, false)), "GPU without ES configs: WARP");
  Check(KeepHardwareDevice(Display(false, basic_render, true, true)),
        "Basic Render Driver with ES 3.0 is kept (it is WARP already)");
  Check(!KeepHardwareDevice(Display(false, basic_render, false, true)),
        "software adapter without ES 3.0: WARP");
  Check(!KeepHardwareDevice(DisplayFacts{}), "no hardware device: WARP");
  Check(!KeepHardwareDevice(Display(true, basic_render, true, true)), "a WARP device is not the hardware one");

  if (failures != 0) {
    std::fprintf(stderr, "egl gles version: %d failed\n", failures);
    return 1;
  }
  std::puts("egl gles version policy all passed");
  return 0;
}
