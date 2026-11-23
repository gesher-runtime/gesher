// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Real Windows gate: window activation/loss, native generation, session state and handle ownership.
#include "devsvc_win32.h"
#include <windows.h>
#include <wtsapi32.h>
#include <cstdio>
#include <cstdlib>

namespace {
int checks;
void check(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", message, GetLastError()); std::exit(1); }
  ++checks;
}
void pump() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
int sessionState() {
  DWORD session = 0, size = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return -1;
  LPWSTR buffer = nullptr;
  bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &buffer, &size) != FALSE;
  int state = -1;
  if (queried && buffer != nullptr && size >= sizeof(WTSINFOEXW)) {
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
    if (info->Level == 1 && info->Data.WTSInfoExLevel1.SessionId == session) {
      if (info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK) state = 0;
      if (info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK) state = 1;
    }
  }
  if (buffer != nullptr) WTSFreeMemory(buffer);
  return state;
}
}
int main() {
  HWND previous = GetForegroundWindow();
  HWND window = CreateWindowExW(0, L"STATIC", L"Gesher clipboard native focus contract", WS_OVERLAPPEDWINDOW,
      80, 80, 400, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(window != nullptr, "Create an actual top-level window");
  ShowWindow(window, SW_SHOW); UpdateWindow(window);
  for (int i = 0; i < 200 && GetForegroundWindow() != window; ++i) {
    SetForegroundWindow(window); pump(); Sleep(10);
  }
  check(GetForegroundWindow() == window, "The fixture acquired actual OS foreground activation");
  uint32_t pid = 0; uint64_t created = 0;
  check(gesher::devsvc::ForegroundProcess(&pid, &created), "Native foreground observation is available");
  check(pid == GetCurrentProcessId(), "Native foreground PID matches the activated window");
  FILETIME start{}, exit{}, kernel{}, user{};
  check(GetProcessTimes(GetCurrentProcess(), &start, &exit, &kernel, &user) != FALSE, "Read the actual process lifetime");
  uint64_t expected = (static_cast<uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime;
  check(created == expected, "Foreground identity matches native process creation FILETIME");
  check(gesher::devsvc::SessionLockState() == sessionState(), "Native lock state matches the server session's actual WTS state");
  DWORD before = 0, after = 0;
  check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "Read native resource baseline");
  for (int i = 0; i < 200; ++i) { gesher::devsvc::ForegroundProcess(&pid, &created); gesher::devsvc::SessionLockState(); }
  check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after <= before + 1, "Repeated queries release process and WTS resources");
  ShowWindow(window, SW_HIDE);
  for (int i = 0; i < 200 && GetForegroundWindow() == window; ++i) { pump(); Sleep(10); }
  check(GetForegroundWindow() != window, "The fixture lost actual OS foreground activation");
  bool known = gesher::devsvc::ForegroundProcess(&pid, &created);
  check(!known || pid != GetCurrentProcessId(), "A hidden prior foreground window does not retain focus authority");
  DestroyWindow(window);
  if (previous != nullptr && IsWindow(previous)) SetForegroundWindow(previous);
  std::printf("Clipboard Windows access all passed: %d checks\n", checks);
}
