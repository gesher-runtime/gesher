// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "home_region_win.h"

#include <windows.h>

namespace dev {
namespace {

bool Alpha2(const wchar_t* name, char16_t out[3], unsigned long* last_error) {
  if (last_error != nullptr) *last_error = 0;
  if (name[0] == L'\0' || name[1] == L'\0' || name[2] != L'\0') return false;
  for (int i = 0; i < 2; ++i) {
    wchar_t c = name[i];
    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    if (c < L'A' || c > L'Z') return false;
    out[i] = static_cast<char16_t>(c);
  }
  out[2] = u'\0';
  // "XX": Windows' code for a location without an ISO 3166-1 code.
  return !(out[0] == u'X' && out[1] == u'X');
}

}  // namespace

bool HomeRegion(char16_t out[3], unsigned long* last_error) {
  using GetUserDefaultGeoNameFn = int(WINAPI*)(LPWSTR, int);
  static const auto get_name = reinterpret_cast<GetUserDefaultGeoNameFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetUserDefaultGeoName")));
  wchar_t name[16] = {};
  if (get_name != nullptr) {
    // "the two-letter ISO 3166-1 code ... if available. Otherwise, a UN M.49 numeric code."
    if (get_name(name, 16) <= 0) {
      if (last_error != nullptr) *last_error = GetLastError();
      return false;
    }
    return Alpha2(name, out, last_error);
  }
  GEOID geo = GetUserGeoID(GEOCLASS_NATION);
  if (geo == GEOID_NOT_AVAILABLE) {
    if (last_error != nullptr) *last_error = 0;
    return false;
  }
  if (GetGeoInfoW(geo, GEO_ISO2, name, 16, 0) <= 0) {
    if (last_error != nullptr) *last_error = GetLastError();
    return false;
  }
  return Alpha2(name, out, last_error);
}

}  // namespace dev
