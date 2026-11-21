// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include "devsvc_win32.h"
#include <windows.h>
#include <wtsapi32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int checks, open_handles, allocations, foreground_calls, owner_calls;
bool open_ok, times_ok, session_ok, query_ok, allocate_buffer;
DWORD owner_pid, owner_thread, wait_result, bytes, level, session_id, reply_session;
LONG lock_flags;
uint64_t creation;
HWND foreground, second_foreground;
DWORD second_owner, second_thread;
void check(bool condition, const char* what) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
  ++checks;
}
void reset() {
  check(open_handles == 0 && allocations == 0, "No retained native resources");
  open_ok = times_ok = session_ok = query_ok = allocate_buffer = true;
  owner_pid = second_owner = 501; owner_thread = second_thread = 41;
  wait_result = WAIT_TIMEOUT; creation = 133700000000000000ULL;
  foreground = second_foreground = reinterpret_cast<HWND>(0x100);
  foreground_calls = owner_calls = 0;
  session_id = reply_session = 3; bytes = sizeof(WTSINFOEXW); level = 1; lock_flags = WTS_SESSIONSTATE_UNLOCK;
}
void focus(bool expected, const char* what) {
  uint32_t pid = 99; uint64_t born = 99;
  check(gesher::devsvc::ForegroundProcess(&pid, &born) == expected, what);
  check(expected ? pid == owner_pid && born == creation : pid == 0 && born == 0, "Unknown focus never returns a stale owner");
  check(open_handles == 0, "Foreground query closes the process handle");
}
}
HWND GetForegroundWindow() { return foreground_calls++ == 0 ? foreground : second_foreground; }
DWORD GetWindowThreadProcessId(HWND window, DWORD* pid) {
  check(window == foreground, "Only the observed window is queried");
  bool first = owner_calls++ == 0; *pid = first ? owner_pid : second_owner; return first ? owner_thread : second_thread;
}
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
  check(access == (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE) && !inherit && pid == owner_pid, "Minimal noninheritable process access");
  if (!open_ok) return nullptr;
  ++open_handles;
  return reinterpret_cast<HANDLE>(0x200);
}
BOOL GetProcessTimes(HANDLE, FILETIME* start, FILETIME*, FILETIME*, FILETIME*) {
  start->dwLowDateTime = static_cast<DWORD>(creation); start->dwHighDateTime = static_cast<DWORD>(creation >> 32); return times_ok;
}
DWORD WaitForSingleObject(HANDLE, DWORD timeout) { check(timeout == 0, "Liveness query never blocks"); return wait_result; }
BOOL CloseHandle(HANDLE) { --open_handles; return true; }
DWORD GetCurrentProcessId() { return 77; }
BOOL ProcessIdToSessionId(DWORD pid, DWORD* session) { check(pid == 77, "Query the server's own session"); *session = session_id; return session_ok; }
BOOL WTSQuerySessionInformationW(HANDLE server, DWORD session, int kind, LPWSTR* data, DWORD* size) {
  check(server == WTS_CURRENT_SERVER_HANDLE && session == session_id && kind == WTSSessionInfoEx, "Actual local session information class");
  if (allocate_buffer) {
    auto* info = static_cast<WTSINFOEXW*>(std::calloc(1, sizeof(WTSINFOEXW))); ++allocations;
    info->Level = level; info->Data.WTSInfoExLevel1.SessionId = reply_session; info->Data.WTSInfoExLevel1.SessionFlags = lock_flags;
    *data = reinterpret_cast<LPWSTR>(info);
  } else *data = nullptr;
  *size = bytes; return query_ok;
}
void WTSFreeMemory(void* data) { --allocations; std::free(data); }
int main() {
  reset(); focus(true, "Stable Windows foreground process");
  reset(); foreground = nullptr; focus(false, "No foreground window");
  reset(); owner_thread = 0; focus(false, "Destroyed window thread");
  reset(); owner_pid = 0; focus(false, "Unknown window owner");
  reset(); open_ok = false; focus(false, "Unqueryable foreground process");
  reset(); times_ok = false; focus(false, "Creation time unavailable");
  reset(); wait_result = WAIT_OBJECT_0; focus(false, "Exited process");
  reset(); wait_result = WAIT_FAILED; focus(false, "Liveness query failure");
  reset(); creation = 0; focus(false, "Unknown process generation");
  reset(); second_foreground = reinterpret_cast<HWND>(0x101); focus(false, "Activation changed while sampling");
  reset(); second_owner++; focus(false, "Window owner changed while sampling");
  reset(); second_thread++; focus(false, "Window thread changed while sampling");
  reset(); check(gesher::devsvc::SessionLockState() == 0, "Actual unlocked session flag");
  reset(); lock_flags = WTS_SESSIONSTATE_LOCK; check(gesher::devsvc::SessionLockState() == 1, "Actual locked session flag");
  reset(); lock_flags = -1; check(gesher::devsvc::SessionLockState() == -1, "Unknown lock flag");
  reset(); lock_flags = 42; check(gesher::devsvc::SessionLockState() == -1, "Unrecognized lock flag");
  reset(); session_ok = false; check(gesher::devsvc::SessionLockState() == -1, "Session lookup failure");
  reset(); query_ok = false; check(gesher::devsvc::SessionLockState() == -1, "Query failure frees a returned buffer");
  reset(); allocate_buffer = false; check(gesher::devsvc::SessionLockState() == -1, "Null query buffer");
  reset(); bytes--; check(gesher::devsvc::SessionLockState() == -1, "Truncated information");
  reset(); level = 2; check(gesher::devsvc::SessionLockState() == -1, "Unsupported information level");
  reset(); reply_session++; check(gesher::devsvc::SessionLockState() == -1, "Session identity changed");
  reset(); std::printf("Clipboard native boundary all passed: %d checks\n", checks);
}
