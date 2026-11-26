// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Manual probe: prints Windows' "Country or region" as CountryDetectorService reads it,
// then prints it again on every WM_SETTINGCHANGE for SECONDS (default 120), so that a
// person can change Settings > Time & language > Language & region > Country or region
// and see what Gesher's listeners would be told. No window is shown.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
//      compat\devsvc\home_region_win.cc tests\country-detector\home_region_probe.cc
//      /Fe:home-region-probe.exe user32.lib
#include "../../compat/devsvc/home_region_win.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

void Print(const char* when) {
  char16_t region[3] = {};
  unsigned long error = 0;
  if (dev::HomeRegion(region, &error)) {
    std::printf("%s: %c%c (COUNTRY_SOURCE_LOCALE)\n", when, static_cast<char>(region[0]),
                static_cast<char>(region[1]));
  } else {
    std::printf("%s: none (error %lu): Gesher answers with the default Locale's country\n", when,
                error);
  }
  std::fflush(stdout);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SETTINGCHANGE) {
    const wchar_t* area = reinterpret_cast<const wchar_t*>(lp);
    std::printf("WM_SETTINGCHANGE %ls\n", area != nullptr ? area : L"(null)");
    Print("  region");
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = argc > 1 ? std::atoi(argv[1]) : 120;
  Print("region");
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherHomeRegionProbe";
  RegisterClassExW(&wc);
  // A top-level window that is never shown: message-only windows get no broadcasts.
  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"probe",
                              WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
  if (hwnd == nullptr) return 1;
  std::printf("watching for %d s: change Country or region in Settings now\n", seconds);
  std::fflush(stdout);
  ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  MSG msg;
  while (GetTickCount64() < end) {
    DWORD left = static_cast<DWORD>(end - GetTickCount64());
    if (MsgWaitForMultipleObjects(0, nullptr, FALSE, left, QS_ALLINPUT) == WAIT_OBJECT_0) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
  DestroyWindow(hwnd);
  return 0;
}
