// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows host test of compat/devsvc/home_region_win.cc (no UI, no settings changed):
// the region it reports is Windows' own "Country or region", read back independently
// through GetUserDefaultGeoName and GetUserGeoID/GetGeoInfoW; a non-ISO answer is "none".
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
//      compat\devsvc\home_region_win.cc tests\country-detector\home_region_windows_test.cc
//      /Fe:home-region-test.exe
#include "../../compat/devsvc/home_region_win.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

bool IsAlpha2(const wchar_t* s) {
  return std::wcslen(s) == 2 && iswupper(s[0]) && iswupper(s[1]) && s[0] < 0x80 && s[1] < 0x80;
}

}  // namespace

int main() {
  char16_t region[3] = {u'?', u'?', u'?'};
  unsigned long error = 0;
  bool have = dev::HomeRegion(region, &error);

  using GetUserDefaultGeoNameFn = int(WINAPI*)(LPWSTR, int);
  const auto get_name = reinterpret_cast<GetUserDefaultGeoNameFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetUserDefaultGeoName")));
  wchar_t name[16] = {};
  int n = get_name != nullptr ? get_name(name, 16) : 0;
  wchar_t iso2[16] = {};
  GEOID geo = GetUserGeoID(GEOCLASS_NATION);
  int m = geo != GEOID_NOT_AVAILABLE ? GetGeoInfoW(geo, GEO_ISO2, iso2, 16, 0) : 0;
  std::printf("info GetUserDefaultGeoName: %ls (%d)\n", n > 0 ? name : L"(failed)", n);
  std::printf("info GetUserGeoID: %ld GEO_ISO2: %ls\n", static_cast<long>(geo), m > 0 ? iso2 : L"(none)");

  if (have) {
    std::printf("info HomeRegion: %c%c\n", static_cast<char>(region[0]), static_cast<char>(region[1]));
    Check(region[2] == u'\0' && region[0] >= u'A' && region[0] <= u'Z' && region[1] >= u'A' &&
              region[1] <= u'Z',
          "two upper-case ASCII letters");
    Check(!(region[0] == u'X' && region[1] == u'X'), "never XX");
    wchar_t mine[3] = {static_cast<wchar_t>(region[0]), static_cast<wchar_t>(region[1]), 0};
    if (get_name != nullptr) {
      Check(n > 0 && _wcsicmp(name, mine) == 0, "the same as GetUserDefaultGeoName");
    }
    Check(m > 0 && _wcsicmp(iso2, mine) == 0, "the same as GetUserGeoID's GEO_ISO2");
  } else {
    std::printf("info HomeRegion: none (error %lu)\n", error);
    if (get_name != nullptr) {
      Check(n <= 0 || !IsAlpha2(name) || std::wcscmp(name, L"XX") == 0,
            "none only when Windows has no two-letter region");
    } else {
      Check(m <= 0 || !IsAlpha2(iso2), "none only when Windows has no two-letter region");
    }
  }
  // Repeatable: the same answer twice.
  char16_t again[3] = {};
  bool have_again = dev::HomeRegion(again, nullptr);
  Check(have_again == have && (!have || (again[0] == region[0] && again[1] == region[1])),
        "the same answer twice");

  std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "home-region PASSED" : "home-region FAILED",
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
