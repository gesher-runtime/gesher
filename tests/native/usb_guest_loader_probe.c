// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// CI-only loader control. Neither variant has a C runtime startup dependency.
#include <windows.h>

#ifdef GESHER_PROBE_USER32
static DWORD WindowsError(void) {
  const DWORD status = GetLastError();
  return status == ERROR_SUCCESS ? ERROR_GEN_FAILURE : status;
}
#endif

void WINAPI GuestLoaderProbeEntry(void) {
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
