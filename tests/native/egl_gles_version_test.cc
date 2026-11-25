// Portable tests of the default EGL display's device choice and the OpenGL ES
// version it gives and the device declares (compat/bionic/egl_gles_version.h,
// docs/system-features.md): the system server's probe declares ES 3.0 only on
// a hardware Direct3D 11 device, and every later process gives exactly the
// boot's declaration, whatever its adapter has become.
//
// Built with -DGESHER_EGL_VERSION_NEGATIVE_CONTROL, the checks run against the
// policy of commit 7d0bd340d (each process re-chooses from its own adapter,
// ignoring the boot's declaration) and must fail
// (tools/test_hwui_renderer_policy.py expects that).
#include "../../compat/bionic/egl_gles_version.h"
#include "../../compat/posix/include/gesher/gles_version_property.h"

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
// Test-only: 7d0bd340d's policy, the declaration ignored.
bool Accept(const DisplayFacts& f, int) {
  const int gives = ConfigVersion(f);
  return gives != kGlEsVersionUndefined && (HardwareDisplay(f) || gives != kGlEsVersion30);
}
int Declared(const DisplayFacts& f, int d) { return Accept(f, d) ? ConfigVersion(f) : kGlEsVersionUndefined; }
Attempt Warp(int) { return Attempt{true, true}; }
Attempt Hardware(int) { return Attempt{false, false}; }
#else
bool Accept(const DisplayFacts& f, int d) { return AcceptDisplay(f, d); }
int Declared(const DisplayFacts& f, int d) { return DeclaredGlEsVersion(f, d); }
Attempt Warp(int d) { return WarpAttempt(d); }
Attempt Hardware(int d) { return HardwareAttempt(d); }
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

}  // namespace

int main() {
  const AdapterIdentity intel = Adapter(0x8086, 0x9a49);
  const AdapterIdentity nvidia = Adapter(0x10de, 0x2882);
  const AdapterIdentity basic_render = Adapter(kMicrosoftVendorId, kBasicRenderDriverDeviceId,
                                               kDxgiAdapterFlagSoftware);
  const AdapterIdentity basic_render_unflagged = Adapter(kMicrosoftVendorId, kBasicRenderDriverDeviceId);
  const AdapterIdentity flagged_software = Adapter(0x1234, 0x5678, kDxgiAdapterFlagSoftware);
  const AdapterIdentity unknown;  // the device query failed
  constexpr int kNone = kGlEsVersionUndefined;

  // The system server's probe (nothing declared yet).
  Check(Declared(Display(false, intel, true, true), kNone) == kGlEsVersion30, "probe: GPU with ES 3.0: 3.0");
  Check(Declared(Display(false, nvidia, true, true), kNone) == kGlEsVersion30, "probe: second vendor: 3.0");
  Check(Declared(Display(false, intel, false, true), kNone) == kGlEsVersion20, "probe: GPU without ES 3.0: 2.0");
  Check(Declared(Display(true, basic_render, false, true), kNone) == kGlEsVersion20, "probe: capped WARP: 2.0");
  Check(!Accept(Display(true, basic_render, true, true), kNone), "probe: uncapped WARP refused");
  Check(!Accept(Display(false, basic_render, true, true), kNone), "probe: Basic Render Driver as hardware refused");
  Check(!Accept(Display(false, basic_render_unflagged, true, true), kNone), "probe: Basic Render Driver by PCI id refused");
  Check(!Accept(Display(false, flagged_software, true, true), kNone), "probe: adapter flagged software refused");
  Check(!Accept(Display(false, unknown, true, true), kNone), "probe: unidentified adapter with ES 3.0 refused");
  Check(Hardware(kNone).capped == false && Warp(kNone).capped == true, "probe: GPU uncapped, WARP capped");
  Check(KeepHardwareDevice(Display(false, intel, false, true), kNone), "probe: GPU with ES 2.0 only kept, not WARP");
  Check(!KeepHardwareDevice(Display(false, basic_render, true, true), kNone), "probe: Basic Render Driver not kept");

  // A process of a boot that declared 3.0 (the console's GPU at boot): its GPU
  // if it still gives ES 3.0, else WARP uncapped, which gives ES 3.0, e.g. after
  // a remote desktop reconnect on the Basic Render Driver or GESHER_EGL_DEVICE=warp.
  Check(Hardware(kGlEsVersion30).capped == false && Warp(kGlEsVersion30).capped == false,
        "declared 3.0: neither device capped");
  Check(KeepHardwareDevice(Display(false, intel, true, true), kGlEsVersion30), "declared 3.0: the GPU kept");
  Check(!KeepHardwareDevice(Display(false, basic_render, true, true), kGlEsVersion30),
        "declared 3.0: the Basic Render Driver as hardware is not kept (WARP instead)");
  Check(Accept(Display(true, basic_render, true, true), kGlEsVersion30) &&
            Declared(Display(true, basic_render, true, true), kGlEsVersion30) == kGlEsVersion30,
        "declared 3.0, no usable GPU: uncapped WARP gives ES 3.0");
  Check(!KeepHardwareDevice(Display(false, intel, false, true), kGlEsVersion30),
        "declared 3.0: a GPU giving only ES 2.0 is not kept");
  Check(!Accept(Display(true, basic_render, false, true), kGlEsVersion30), "declared 3.0: a capped WARP refused");

  // A process of a boot that declared 2.0 (no GPU at boot): capped whatever
  // device it gets now, a GPU included.
  Check(Hardware(kGlEsVersion20).capped && Warp(kGlEsVersion20).capped, "declared 2.0: both devices capped");
  Check(KeepHardwareDevice(Display(false, intel, false, true), kGlEsVersion20) &&
            Declared(Display(false, intel, false, true), kGlEsVersion20) == kGlEsVersion20,
        "declared 2.0: a capped GPU kept, ES 2.0");
  Check(!Accept(Display(false, intel, true, true), kGlEsVersion20),
        "declared 2.0: a GPU still offering ES 3.0 refused");
  Check(Declared(Display(true, basic_render, false, true), kGlEsVersion20) == kGlEsVersion20,
        "declared 2.0: capped WARP gives ES 2.0");

  // Other ro.opengles.version values count as no declaration.
  Check(Declaration(0x30001) == kNone && Declaration(0x10000) == kNone && Declaration(0) == kNone,
        "only 0x20000 and 0x30000 are declarations");

  // Context versions on the default display: never above what it gives.
  Check(ContextVersionAllowed(kGlEsVersion30, 3, 0), "3.0 display: ES 3.0 context");
  Check(!ContextVersionAllowed(kGlEsVersion30, 3, 1) && !ContextVersionAllowed(kGlEsVersion30, 3, 2),
        "3.0 display: no ES 3.1/3.2 context");
  Check(!ContextVersionAllowed(kGlEsVersion20, 3, 0), "2.0 display: no ES 3 context");
  Check(ContextVersionAllowed(kGlEsVersion20, 2, 0) && ContextVersionAllowed(kGlEsVersion30, 2, 0),
        "ES 2.0 contexts everywhere");
  Check(ContextVersionAllowed(kGlEsVersion20, 1, 0) && ContextVersionAllowed(kGlEsVersion20, 1, 1),
        "ES 1.x contexts everywhere");
  Check(!ContextVersionAllowed(kGlEsVersion30, 4, 0), "no ES 4");

  // The invariant over every combination: a kept display gives exactly what is
  // declared for it; without a declaration, ES 3.0 only on hardware.
  const AdapterIdentity adapters[] = {intel, basic_render, basic_render_unflagged, flagged_software, unknown};
  const int declarations[] = {kNone, kGlEsVersion20, kGlEsVersion30};
  int combinations = 0;
  for (int declared : declarations) {
    for (int display = 0; display < 2; ++display) {
      for (int warp = 0; warp < 2; ++warp) {
        for (const AdapterIdentity& adapter : adapters) {
          for (int es3 = 0; es3 < 2; ++es3) {
            for (int es2 = 0; es2 < 2; ++es2) {
              DisplayFacts f = Display(warp != 0, adapter, es3 != 0, es2 != 0);
              f.display = display != 0;
              ++combinations;
              const int version = Declared(f, declared);
              if (Accept(f, declared)) {
                Check(version == ConfigVersion(f), "kept display: its version is what its configs give");
                if (declared != kNone) Check(version == declared, "kept display: gives exactly the declaration");
                else Check(HardwareDisplay(f) || version != kGlEsVersion30, "probe: ES 3.0 only on hardware");
              } else {
                Check(version == kNone, "refused display: nothing");
              }
            }
          }
        }
      }
    }
  }

  // build.prop's ro.opengles.version: only served versions, in exactly the forms
  // every loader accepts (the same table is in PackageServiceQueriesTest).
  for (const char* served : {"196608", "131072", "0x30000", "0X30000", "0x20000"}) {
    Check(gesher::properties::ServedBuildProperty("ro.opengles.version", served), served);
  }
  for (const char* refused : {"196609", "0x30001", "65536", "#30000", "+196608", "-196608", " 196608", "196608 ", "0x", "", "three", "0x3000G", "1966080000000", "0196608", "00x30000"}) {
    Check(!gesher::properties::ServedBuildProperty("ro.opengles.version", refused), refused);
  }
  Check(gesher::properties::ServedBuildProperty("ro.product.model", "196609"), "other properties kept");
  // What app processes read as the boot's declaration (egl_gles.cc BootDeclaration:
  // ParseGlEsVersion, then Declaration) agrees with what the loaders keep.
  for (const char* value : {"196608", "131072", "0x30000", "0X30000", "0x20000", "196609", "0x30001",
                            "#30000", "+196608", " 196608", "0196608", "00x30000", "three", ""}) {
    long long parsed = 0;
    const bool declared = gesher::properties::ParseGlEsVersion(value, &parsed) &&
                          Declaration(static_cast<int>(parsed)) != kGlEsVersionUndefined;
    Check(declared == gesher::properties::ServedBuildProperty("ro.opengles.version", value), value);
  }

  if (failures != 0) {
    std::fprintf(stderr, "egl gles version: %d failed\n", failures);
    return 1;
  }
  std::printf("egl gles version policy all passed (%d combinations)\n", combinations);
  return 0;
}
