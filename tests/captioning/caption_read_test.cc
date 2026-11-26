// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Exercises the production caption reader against a scripted WinRT boundary.
// Link caption_win.cc, NOT winrt.cc. The separate caption_windows_test still
// checks the real Windows settings and never accepts a failed Read as success.
// MSVC derives the fake from the Windows SDK interface, checking its actual ABI.
#include "../../compat/devices/caption_win.h"
#include "../../compat/devices/winrt.h"

#if defined(_MSC_VER)
#include <windows.media.closedcaptioning.h>
#endif

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace captions = gesher::devices::captions;
namespace winrt = gesher::devices::winrt;

namespace {

#if defined(_MSC_VER)
namespace sdk = ABI::Windows::Media::ClosedCaptioning;
using Statics = sdk::IClosedCaptionPropertiesStatics;
using CaptionColor = sdk::ClosedCaptionColor;
using CaptionOpacity = sdk::ClosedCaptionOpacity;
using CaptionSize = sdk::ClosedCaptionSize;
using CaptionStyle = sdk::ClosedCaptionStyle;
using CaptionEffect = sdk::ClosedCaptionEdgeEffect;
using Color = ABI::Windows::UI::Color;
using RuntimeString = ::HSTRING;
using RuntimeTrust = ::TrustLevel;
#else
// The pinned MinGW generated headers collide on IReference<BYTE>/boolean.
// This fallback uses windows.media.closedcaptioning.idl's exact vtable order;
// the CI build above checks the same fake against Microsoft's SDK declaration.
using CaptionColor = int32_t;
using CaptionOpacity = int32_t;
using CaptionSize = int32_t;
using CaptionStyle = int32_t;
using CaptionEffect = int32_t;
struct Color { BYTE A, R, G, B; };
using RuntimeString = winrt::HSTRING;
using RuntimeTrust = int;
struct Statics : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_FontColor(CaptionColor*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ComputedFontColor(Color*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontOpacity(CaptionOpacity*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontSize(CaptionSize*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontStyle(CaptionStyle*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontEffect(CaptionEffect*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(CaptionColor*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ComputedBackgroundColor(Color*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BackgroundOpacity(CaptionOpacity*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RegionColor(CaptionColor*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ComputedRegionColor(Color*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RegionOpacity(CaptionOpacity*) = 0;
};
#endif

static_assert(sizeof(Color) == 4 && sizeof(CaptionColor) == 4 &&
              sizeof(CaptionOpacity) == 4, "WinRT color and enum ABI");
static_assert(sizeof(captions::Properties) == 12 * sizeof(uint32_t),
              "Properties has no padding in the unchanged-output comparison");

const IID kStaticsIid = {
    0x10aa1f84, 0xcc30, 0x4141, {0xb5, 0x03, 0x52, 0x72, 0x28, 0x9e, 0x0c, 0x20}};
constexpr wchar_t kClass[] = L"Windows.Media.ClosedCaptioning.ClosedCaptionProperties";
const char* const kChannels[] = {"font", "background", "region"};
const uint32_t kArgb[] = {0x7a123456u, 0xab234567u, 0xcd345678u};
uint32_t captions::Properties::* const kComputed[] = {
    &captions::Properties::computed_font_color,
    &captions::Properties::computed_background_color,
    &captions::Properties::computed_region_color};
int32_t captions::Properties::* const kColors[] = {
    &captions::Properties::font_color, &captions::Properties::background_color,
    &captions::Properties::region_color};
int32_t captions::Properties::* const kOpacities[] = {
    &captions::Properties::font_opacity, &captions::Properties::background_opacity,
    &captions::Properties::region_opacity};

int g_checks = 0, g_failures = 0;
int g_apartments = 0, g_factories = 0;
HRESULT g_apartment_hr = S_OK, g_factory_hr = S_OK;
bool g_activation_matches = false;

void Check(bool ok, const char* test, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", test, what);
}

struct Component {
  int32_t color = 0, opacity = 0;
  Color computed = {};
  HRESULT color_hr = S_OK, opacity_hr = S_OK, computed_hr = E_INVALIDARG;
  int computed_calls = 0;
};

class FakeStatics final : public Statics {
 public:
  Component components[3];
  ULONG refs = 1;  // The fixture owns this reference; activation hands Read another.
  int releases = 0;

  FakeStatics() {
    components[0].computed = {0x7a, 0x12, 0x34, 0x56};
    components[1].computed = {0xab, 0x23, 0x45, 0x67};
    components[2].computed = {0xcd, 0x34, 0x56, 0x78};
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, kStaticsIid)) return E_NOINTERFACE;
    *out = static_cast<Statics*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { ++releases; return --refs; }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(RuntimeString*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(RuntimeTrust*) override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE get_FontColor(CaptionColor* out) override { return ReadColor(0, out); }
  HRESULT STDMETHODCALLTYPE get_ComputedFontColor(Color* out) override { return ReadComputed(0, out); }
  HRESULT STDMETHODCALLTYPE get_FontOpacity(CaptionOpacity* out) override { return ReadOpacity(0, out); }
  HRESULT STDMETHODCALLTYPE get_FontSize(CaptionSize* out) override {
    *out = static_cast<CaptionSize>(2); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_FontStyle(CaptionStyle* out) override {
    *out = static_cast<CaptionStyle>(6); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_FontEffect(CaptionEffect* out) override {
    *out = static_cast<CaptionEffect>(4); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_BackgroundColor(CaptionColor* out) override { return ReadColor(1, out); }
  HRESULT STDMETHODCALLTYPE get_ComputedBackgroundColor(Color* out) override { return ReadComputed(1, out); }
  HRESULT STDMETHODCALLTYPE get_BackgroundOpacity(CaptionOpacity* out) override { return ReadOpacity(1, out); }
  HRESULT STDMETHODCALLTYPE get_RegionColor(CaptionColor* out) override { return ReadColor(2, out); }
  HRESULT STDMETHODCALLTYPE get_ComputedRegionColor(Color* out) override { return ReadComputed(2, out); }
  HRESULT STDMETHODCALLTYPE get_RegionOpacity(CaptionOpacity* out) override { return ReadOpacity(2, out); }

 private:
  HRESULT ReadColor(int channel, CaptionColor* out) {
    *out = static_cast<CaptionColor>(components[channel].color);
    return components[channel].color_hr;
  }
  HRESULT ReadOpacity(int channel, CaptionOpacity* out) {
    *out = static_cast<CaptionOpacity>(components[channel].opacity);
    return components[channel].opacity_hr;
  }
  HRESULT ReadComputed(int channel, Color* out) {
    ++components[channel].computed_calls;
    *out = components[channel].computed;
    return components[channel].computed_hr;
  }
};

FakeStatics* g_statics = nullptr;

captions::Properties Sentinel() {
  return {91, 0x12345678u, 92, 93, 94, 95, 96, 0x23456789u, 97, 98, 0x3456789au, 99};
}

bool Same(const captions::Properties& a, const captions::Properties& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void Reset(FakeStatics* statics) {
  g_statics = statics;
  g_apartments = g_factories = 0;
  g_apartment_hr = g_factory_hr = S_OK;
  g_activation_matches = false;
}

HRESULT ReadFixture(FakeStatics& statics, captions::Properties* out, const char* test) {
  Reset(&statics);
  HRESULT hr = captions::Read(out);
  Check(g_apartments == 1 && g_factories == 1 && g_activation_matches,
        test, "activates the real caption class and interface once");
  Check(statics.releases == 1 && statics.refs == 1,
        test, "releases the acquired factory reference exactly once");
  return hr;
}

void Defaults() {
  FakeStatics statics;  // Every computed getter would return E_INVALIDARG.
  captions::Properties out = Sentinel();
  const char* test = "default colors and opacities";
  Check(ReadFixture(statics, &out, test) == S_OK, test, "Read returns S_OK");
  Check(statics.components[0].computed_calls == 0 && statics.components[1].computed_calls == 0 &&
            statics.components[2].computed_calls == 0, test, "never calls a computed getter");
  Check(out.font_color == 0 && out.font_opacity == 0 && out.background_color == 0 &&
            out.background_opacity == 0 && out.region_color == 0 && out.region_opacity == 0 &&
            out.computed_font_color == 0 && out.computed_background_color == 0 &&
            out.computed_region_color == 0, test, "preserves default choices without invented colors");
  Check(out.font_size == 2 && out.font_style == 6 && out.font_effect == 4,
        test, "still reads size, style and effect");
}

void Components() {
  for (int channel = 0; channel != 3; ++channel) {
    // Reach each default component after reading specified preceding components.
    FakeStatics defaults;
    for (int other = 0; other != 3; ++other) {
      if (other == channel) continue;
      defaults.components[other].color = 1;
      defaults.components[other].computed_hr = S_OK;
    }
    captions::Properties out = Sentinel();
    Check(ReadFixture(defaults, &out, kChannels[channel]) == S_OK,
          kChannels[channel], "a default pair skips its invalid computed getter");
    Check(defaults.components[channel].computed_calls == 0 && out.*kComputed[channel] == 0,
          kChannels[channel], "default pair leaves its unused computed color zero");

    for (int specified = 1; specified != 4; ++specified) {
      for (HRESULT result : {S_OK, E_ACCESSDENIED}) {
        FakeStatics statics;
        Component& component = statics.components[channel];
        component.color = (specified & 1) != 0 ? 1 : 0;
        component.opacity = (specified & 2) != 0 ? 3 : 0;
        component.computed_hr = result;
        const captions::Properties before = Sentinel();
        out = before;
        HRESULT hr = ReadFixture(statics, &out, kChannels[channel]);
        Check(hr == result && component.computed_calls == 1,
              kChannels[channel], "color-only, opacity-only or both require the computed getter");
        if (SUCCEEDED(result)) {
          Check(out.*kComputed[channel] == kArgb[channel] &&
                    out.*kColors[channel] == component.color &&
                    out.*kOpacities[channel] == component.opacity,
                kChannels[channel], "preserves the Windows ARGB and specified enum values");
        } else {
          Check(Same(out, before), kChannels[channel], "computed failure leaves all output unchanged");
        }
      }
    }

    for (bool fail_opacity : {false, true}) {
      FakeStatics statics;
      Component& component = statics.components[channel];
      component.color = 1;
      component.computed_hr = S_OK;
      if (fail_opacity) component.opacity_hr = E_ABORT;
      else component.color_hr = E_ABORT;
      const captions::Properties before = Sentinel();
      out = before;
      Check(ReadFixture(statics, &out, kChannels[channel]) == E_ABORT && Same(out, before),
            kChannels[channel], "color/opacity getter failure propagates with unchanged output");
      Check(component.computed_calls == 0,
            kChannels[channel], "failed enum is never used to request a computed color");
    }
  }
}

void EarlyFailures() {
  FakeStatics statics;
  Reset(&statics);
  Check(captions::Read(nullptr) == E_POINTER && g_apartments == 0 && g_factories == 0,
        "null output", "fails before activating Windows");
  const captions::Properties before = Sentinel();
  captions::Properties out = before;
  g_apartment_hr = E_FAIL;
  Check(captions::Read(&out) == E_FAIL && Same(out, before) && g_factories == 0,
        "apartment failure", "propagates without activation or output mutation");
  Reset(&statics);
  g_factory_hr = E_ACCESSDENIED;
  Check(captions::Read(&out) == E_ACCESSDENIED && Same(out, before) &&
            g_factories == 1 && statics.releases == 0 && statics.refs == 1,
        "activation failure", "does not release a reference it never acquired");
}

}  // namespace

// The only substituted boundary. Production Read and its vtable calls are linked
// unchanged from caption_win.cc; no Windows setting or real apartment is modified.
namespace gesher::devices::winrt {
HRESULT EnsureApartment() {
  ++g_apartments;
  return g_apartment_hr;
}
HRESULT GetActivationFactory(const wchar_t* class_name, const IID& iid, void** factory) {
  ++g_factories;
  *factory = nullptr;
  g_activation_matches = std::wcscmp(class_name, kClass) == 0 && IsEqualIID(iid, kStaticsIid);
  if (!g_activation_matches) return E_NOINTERFACE;
  if (FAILED(g_factory_hr)) return g_factory_hr;
  *factory = static_cast<Statics*>(g_statics);
  g_statics->AddRef();
  return S_OK;
}
}  // namespace gesher::devices::winrt

int main() {
#if defined(_MSC_VER)
  Check(IsEqualIID(kStaticsIid, __uuidof(Statics)), "SDK ABI", "uses the SDK caption interface IID");
#endif
  Defaults();
  Components();
  EarlyFailures();
  std::printf("caption Read %s: %d checks, %d failures\n",
              g_failures == 0 ? "PASSED" : "FAILED", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
