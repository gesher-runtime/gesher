// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "devsvc_win32.h"

#include <windows.h>
#include <wtsapi32.h>

namespace gesher::devsvc {

bool ForegroundProcess(uint32_t* pid, uint64_t* created) {
  *pid = 0;
  *created = 0;
  HWND window = GetForegroundWindow();
  if (window == nullptr) return false;
  DWORD owner = 0;
  DWORD thread = GetWindowThreadProcessId(window, &owner);
  if (thread == 0 || owner == 0) return false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner);
  if (process == nullptr) return false;
  FILETIME start{}, exit{}, kernel{}, user{};
  DWORD current_owner = 0;
  // A PID alone is insufficient: an exited application's PID can be reused by
  // an unrelated foreground process. Hold its process object while sampling,
  // and reject activation/owner changes during the query.
  bool valid = GetProcessTimes(process, &start, &exit, &kernel, &user)
      && WaitForSingleObject(process, 0) == WAIT_TIMEOUT
      && GetWindowThreadProcessId(window, &current_owner) == thread
      && current_owner == owner && GetForegroundWindow() == window;
  CloseHandle(process);
  uint64_t creation = (static_cast<uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime;
  if (!valid || creation == 0) return false;
  *pid = owner;
  *created = creation;
  return true;
}

int SessionLockState() {
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return -1;
  LPWSTR data = nullptr;
  DWORD size = 0;
  BOOL queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session,
      WTSSessionInfoEx, &data, &size);
  int result = -1;
  if (queried && data != nullptr && size >= sizeof(WTSINFOEXW)) {
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(data);
    if (info->Level == 1 && info->Data.WTSInfoExLevel1.SessionId == session) {
      LONG flags = info->Data.WTSInfoExLevel1.SessionFlags;
      if (flags == WTS_SESSIONSTATE_LOCK) result = 1;
      if (flags == WTS_SESSIONSTATE_UNLOCK) result = 0;
    }
  }
  if (data != nullptr) WTSFreeMemory(data);
  return result;
}

}  // namespace gesher::devsvc
