// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// CI-only loader controls without a C runtime startup dependency.
#include <windows.h>

#if defined(GESHER_PROBE_USER32) || defined(GESHER_PROBE_USER32_LATE)
static DWORD WindowsError(void) {
  const DWORD status = GetLastError();
  return status == ERROR_SUCCESS ? ERROR_GEN_FAILURE : status;
}
#endif

void WINAPI GuestLoaderProbeEntry(void) {
#ifdef GESHER_PROBE_USER32_LATE
  // This image reaches its entry with only KERNEL32 imported. Return the
  // exact Win32 loader result from trying USER32 after that point.
  const HMODULE library = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library) ExitProcess(WindowsError());
  FreeLibrary(library);
#endif
#ifdef GESHER_PROBE_USER32
  const HWINSTA station = GetProcessWindowStation();
  if (!station) ExitProcess(WindowsError());
  const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
  if (!desktop) ExitProcess(WindowsError());
  wchar_t name[64];
  DWORD required = 0;
  if (!GetUserObjectInformationW(station, UOI_NAME, name, (DWORD)sizeof(name), &required)) {
    ExitProcess(WindowsError());
  }
  if (CompareStringOrdinal(name, -1, L"WinSta0", -1, TRUE) != CSTR_EQUAL) {
    ExitProcess(ERROR_INVALID_NAME);
  }
  if (!GetUserObjectInformationW(desktop, UOI_NAME, name, (DWORD)sizeof(name), &required)) {
    ExitProcess(WindowsError());
  }
  if (CompareStringOrdinal(name, -1, L"Default", -1, TRUE) != CSTR_EQUAL) {
    ExitProcess(ERROR_INVALID_NAME);
  }
#endif
  ExitProcess(ERROR_SUCCESS);
}
