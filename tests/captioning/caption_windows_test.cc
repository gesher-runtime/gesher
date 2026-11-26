// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows host test of compat/devices/caption_win.cc (no UI, no setting changed): the
// caption style is read through ClosedCaptionProperties with every value inside Windows'
// enums, twice the same; the PropertiesChanged watch starts (or reports E_NOINTERFACE on
// a Windows without it), times out without a change and stops.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /Icompat\devices
//      compat\devices\caption_win.cc compat\devices\winrt.cc tests\captioning\caption_windows_test.cc
//      /Fe:caption-test.exe ole32.lib uuid.lib
#include "../../compat/devices/caption_win.h"
#include "../../compat/devices/winrt.h"

// MinGW's generated WinRT headers collide on IReference<BYTE>/IReference<boolean>.
// The production contracts below still run there; SDK diagnostics use Microsoft's SDK.
#if defined(_MSC_VER)
#include <windows.media.closedcaptioning.h>
#endif

#include <cstdio>

namespace captions = gesher::devices::captions;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

bool In(int value, int last) { return value >= 0 && value <= last; }

// Read every getter through the SDK's own ABI declaration. A combined Read failure
// otherwise hides which Windows property failed, and whether the bridge's hand-written
// interface declaration agrees with the installed SDK.
#if defined(_MSC_VER)
void DumpSdkProperties() {
  namespace sdk = ABI::Windows::Media::ClosedCaptioning;
  sdk::IClosedCaptionPropertiesStatics* settings = nullptr;
  HRESULT hr = gesher::devices::winrt::GetActivationFactory(
      L"Windows.Media.ClosedCaptioning.ClosedCaptionProperties",
      __uuidof(sdk::IClosedCaptionPropertiesStatics), reinterpret_cast<void**>(&settings));
  std::printf("SDK activation hr 0x%08lx\n", static_cast<unsigned long>(hr));
  if (FAILED(hr)) return;
#define READ_ENUM(Name, Type) do { \
    sdk::Type value = {}; \
    HRESULT result = settings->get_##Name(&value); \
    std::printf("SDK " #Name " hr 0x%08lx value %d\n", \
                static_cast<unsigned long>(result), static_cast<int>(value)); \
  } while (false)
#define READ_COLOR(Name) do { \
    ABI::Windows::UI::Color value = {}; \
    HRESULT result = settings->get_##Name(&value); \
    std::printf("SDK " #Name " hr 0x%08lx ARGB %u %u %u %u\n", \
                static_cast<unsigned long>(result), static_cast<unsigned>(value.A), \
                static_cast<unsigned>(value.R), static_cast<unsigned>(value.G), \
                static_cast<unsigned>(value.B)); \
  } while (false)
  READ_ENUM(FontColor, ClosedCaptionColor);
  READ_COLOR(ComputedFontColor);
  READ_ENUM(FontOpacity, ClosedCaptionOpacity);
  READ_ENUM(FontSize, ClosedCaptionSize);
  READ_ENUM(FontStyle, ClosedCaptionStyle);
  READ_ENUM(FontEffect, ClosedCaptionEdgeEffect);
  READ_ENUM(BackgroundColor, ClosedCaptionColor);
  READ_COLOR(ComputedBackgroundColor);
  READ_ENUM(BackgroundOpacity, ClosedCaptionOpacity);
  READ_ENUM(RegionColor, ClosedCaptionColor);
  READ_COLOR(ComputedRegionColor);
  READ_ENUM(RegionOpacity, ClosedCaptionOpacity);
#undef READ_COLOR
#undef READ_ENUM
  settings->Release();
}
#endif

}  // namespace

int main() {
#if defined(_MSC_VER)
  DumpSdkProperties();
#endif
  captions::Properties p = {};
  HRESULT hr = captions::Read(&p);
  std::printf("info Read: hr 0x%08lx\n", static_cast<unsigned long>(hr));
  Check(hr == S_OK, "ClosedCaptionProperties answers");
  if (hr == S_OK) {
    std::printf("info font color %d (computed 0x%08x) opacity %d size %d style %d effect %d\n",
                p.font_color, p.computed_font_color, p.font_opacity, p.font_size, p.font_style,
                p.font_effect);
    std::printf("info background %d (computed 0x%08x) opacity %d; region %d (computed 0x%08x) opacity %d\n",
                p.background_color, p.computed_background_color, p.background_opacity,
                p.region_color, p.computed_region_color, p.region_opacity);
    Check(In(p.font_color, 8) && In(p.background_color, 8) && In(p.region_color, 8),
          "colors are ClosedCaptionColor values");
    Check(In(p.font_opacity, 4) && In(p.background_opacity, 4) && In(p.region_opacity, 4),
          "opacities are ClosedCaptionOpacity values");
    Check(In(p.font_size, 4), "size is a ClosedCaptionSize value");
    Check(In(p.font_style, 7), "style is a ClosedCaptionStyle value");
    Check(In(p.font_effect, 5), "effect is a ClosedCaptionEdgeEffect value");
    captions::Properties again = {};
    Check(captions::Read(&again) == S_OK && again.font_color == p.font_color &&
              again.font_size == p.font_size && again.computed_font_color == p.computed_font_color &&
              again.region_opacity == p.region_opacity,
          "the same answer twice");
  }
  Check(captions::Read(nullptr) == E_POINTER, "no output: E_POINTER");

  hr = captions::StartWatch();
  std::printf("info StartWatch: hr 0x%08lx\n", static_cast<unsigned long>(hr));
  Check(hr == S_OK || hr == E_NOINTERFACE, "PropertiesChanged registers (or is absent on this Windows)");
  if (hr == S_OK) {
    Check(captions::StartWatch() == S_FALSE, "a second watch is the same watch");
    int64_t seen = captions::AwaitChange(0, 0);
    Check(seen >= 0, "the watch runs");
    Check(captions::AwaitChange(seen, 200) == seen, "no change: time-out with the same count");
    captions::StopWatch();
    Check(captions::AwaitChange(seen, 200) == -1, "after StopWatch: -1");
  } else {
    Check(captions::AwaitChange(0, 0) == -1, "no watch: -1");
  }

  std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "captions PASSED" : "captions FAILED",
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
