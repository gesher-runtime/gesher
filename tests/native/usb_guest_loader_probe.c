// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// CI-only loader controls without a C runtime startup dependency.
#include <windows.h>
#include "usb_heap_control_wire.h"

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
  // Explicit separate control: execute the documented heap operation before
  // any USER32 load and report each real API result without packing error bits.
  static const wchar_t heap_argument[] = L" --heap-control=";
  const int heap_length = (int)(sizeof(heap_argument) / sizeof(heap_argument[0])) - 1;
  // Both fixture launchers quote argv[0]. Also accept an unquoted simple
  // image for local controls; never inspect a mode string inside its pathname.
  const wchar_t* arguments = command;
  if (*arguments == L'"') {
    ++arguments;
    while (*arguments && *arguments != L'"') ++arguments;
    if (*arguments == L'"') ++arguments;
  } else {
    while (*arguments && *arguments != L' ' && *arguments != L'\t') ++arguments;
  }
  if (lstrlenW(arguments) > heap_length &&
      CompareStringOrdinal(arguments, heap_length, heap_argument, heap_length, FALSE) == CSTR_EQUAL) {
    GesherHeapControlReport report = {0};
    report.magic = GESHER_HEAP_CONTROL_MAGIC;
    report.version = 1;
    SetLastError(ERROR_SUCCESS);
    const HANDLE heap = HeapCreate(0, 0, 0);
    report.create_error = GetLastError();
    report.create_success = heap != NULL;
    if (heap) {
      report.destroy_called = 1;
      SetLastError(ERROR_SUCCESS);
      report.destroy_success = HeapDestroy(heap) != FALSE;
      report.destroy_error = GetLastError();
    }
    // The controller precreates this exact diagnostic file. Its parent remains
    // protected; this child can only write its explicitly admitted report leaf.
    const HANDLE output = CreateFileW(arguments + heap_length, FILE_WRITE_DATA,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (output == INVALID_HANDLE_VALUE) ExitProcess(WindowsError());
    DWORD written = 0;
    const BOOL complete = WriteFile(output, &report, (DWORD)sizeof(report), &written, NULL);
    const DWORD write_error = complete ? ERROR_SUCCESS : WindowsError();
    const BOOL closed = CloseHandle(output);
    if (!complete || written != sizeof(report)) ExitProcess(write_error ? write_error : ERROR_WRITE_FAULT);
    if (!closed) ExitProcess(WindowsError());
    // Zero means this diagnostic report was delivered, not that HeapCreate or
    // USER32 succeeded. The report contains the separate operation results.
    ExitProcess(ERROR_SUCCESS);
  }
  if (length >= argument_length && CompareStringOrdinal(command + length - argument_length,
          argument_length, wait_argument, argument_length, FALSE) == CSTR_EQUAL) {
    // Only the disposable traced copy waits. The authority resumes it in
    // the normal way, without a debugger modifying its startup suspension.
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (!IsDebuggerPresent()) {
      if (GetTickCount64() >= deadline) ExitProcess(ERROR_TIMEOUT);
      Sleep(10);
    }
    // Arm diagnostic hardware breakpoints after the real entry context.
    // Only the disposable traced copy emits this marker.
    OutputDebugStringA("GESHER_LOADER_ENTRY");
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
