/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <string.h>

/* DWM mirrors the caption without changing client pixels or input coordinates. */
static inline void GesherSetNonclientRtl(HWND window, BOOL rtl) {
  HMODULE module = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (module == NULL) return;
  typedef HRESULT (WINAPI *SetAttribute)(HWND, DWORD, LPCVOID, DWORD);
  FARPROC address = GetProcAddress(module, "DwmSetWindowAttribute");
  SetAttribute set_attribute = NULL;
  memcpy(&set_attribute, &address, sizeof(set_attribute));
  if (set_attribute != NULL)
    set_attribute(window, DWMWA_NONCLIENT_RTL_LAYOUT, &rtl, sizeof(rtl));
  FreeLibrary(module);
}
