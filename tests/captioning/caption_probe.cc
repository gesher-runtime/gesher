// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Manual probe: prints Windows' caption style as Gesher reads it, then again on every
// change Windows reports (PropertiesChanged) for SECONDS (default 120), so that a person
// can change Settings > Accessibility > Captions > Caption style and see the values.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /Icompat\devices
//      compat\devices\caption_win.cc compat\devices\winrt.cc tests\captioning\caption_probe.cc
//      /Fe:caption-probe.exe ole32.lib uuid.lib
#include "../../compat/devices/caption_win.h"

#include <cstdio>
#include <cstdlib>

namespace captions = gesher::devices::captions;

namespace {

void Print() {
  captions::Properties p = {};
  HRESULT hr = captions::Read(&p);
  if (FAILED(hr)) {
    std::printf("read failed: hr 0x%08lx\n", static_cast<unsigned long>(hr));
    return;
  }
  std::printf("font: color %d computed 0x%08x opacity %d size %d style %d effect %d\n", p.font_color,
              p.computed_font_color, p.font_opacity, p.font_size, p.font_style, p.font_effect);
  std::printf("background: color %d computed 0x%08x opacity %d\n", p.background_color,
              p.computed_background_color, p.background_opacity);
  std::printf("region (window): color %d computed 0x%08x opacity %d\n", p.region_color,
              p.computed_region_color, p.region_opacity);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = argc > 1 ? std::atoi(argv[1]) : 120;
  Print();
  HRESULT hr = captions::StartWatch();
  if (FAILED(hr)) {
    std::printf("no PropertiesChanged: hr 0x%08lx\n", static_cast<unsigned long>(hr));
    return 0;
  }
  std::printf("watching for %d s: change the caption style in Settings now\n", seconds);
  std::fflush(stdout);
  ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  int64_t seen = captions::AwaitChange(-2, 0);
  while (GetTickCount64() < end) {
    int64_t now = captions::AwaitChange(seen, 1000);
    if (now < 0) break;
    if (now != seen) {
      seen = now;
      std::printf("PropertiesChanged (%lld)\n", static_cast<long long>(now));
      Print();
    }
  }
  captions::StopWatch();
  return 0;
}
