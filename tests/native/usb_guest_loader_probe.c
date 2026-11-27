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
  static const wchar_t wait_argument[] = L" --wait-for-loader-debugger";
  const wchar_t* command = GetCommandLineW();
  const int length = lstrlenW(command);
  const int argument_length = (int)(sizeof(wait_argument) / sizeof(wait_argument[0])) - 1;
  if (length >= argument_length && CompareStringOrdinal(command + length - argument_length,
          argument_length, wait_argument, argument_length, FALSE) == CSTR_EQUAL) {
    // Only the disposable traced copy waits. The authority resumes it in
    // the normal way, without a debugger modifying its startup suspension.
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (!IsDebuggerPresent()) {
      if (GetTickCount64() >= deadline) ExitProcess(ERROR_TIMEOUT);
      Sleep(10);
    }
  }
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
