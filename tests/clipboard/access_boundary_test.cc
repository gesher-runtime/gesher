// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include "devsvc_win32.h"
#include "device_admin_role.h"
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
bool lock_submit_ok;
int lock_calls, sleep_calls, confirm_after_sleeps;
ULONGLONG ticks;
bool app_role, commandline_ok = true;
int argv_allocations, session_queries;
LPWSTR main_arguments[5];
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
  lock_submit_ok = true; lock_calls = sleep_calls = 0;
  confirm_after_sleeps = -1; ticks = 0;
  session_queries = 0;
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
BOOL ProcessIdToSessionId(DWORD pid, DWORD* session) { ++session_queries; check(pid == 77, "Query the server's own session"); *session = session_id; return session_ok; }
BOOL WTSQuerySessionInformationW(HANDLE server, DWORD session, int kind, LPWSTR* data, DWORD* size) {
  check(server == WTS_CURRENT_SERVER_HANDLE && session == session_id && kind == WTSSessionInfoEx, "Actual local session information class");
  if (allocate_buffer) {
    auto* info = static_cast<WTSINFOEXW*>(std::calloc(1, sizeof(WTSINFOEXW))); ++allocations;
    const LONG observed_flags = lock_calls > 0 && confirm_after_sleeps >= 0
        && sleep_calls >= confirm_after_sleeps ? WTS_SESSIONSTATE_LOCK : lock_flags;
    info->Level = level; info->Data.WTSInfoExLevel1.SessionId = reply_session; info->Data.WTSInfoExLevel1.SessionFlags = observed_flags;
    *data = reinterpret_cast<LPWSTR>(info);
  } else *data = nullptr;
  *size = bytes; return query_ok;
}
void WTSFreeMemory(void* data) { --allocations; std::free(data); }
BOOL LockWorkStation() { ++lock_calls; return lock_submit_ok; }
ULONGLONG GetTickCount64() { return ticks; }
void Sleep(DWORD milliseconds) {
  check(milliseconds == 20, "Session confirmation uses bounded polling intervals");
  ++sleep_calls; ticks += milliseconds;
  check(sleep_calls <= 150, "Session confirmation never waits beyond its deadline");
}
LPWSTR GetCommandLineW() { return const_cast<LPWSTR>(L"actual Windows launch command"); }
LPWSTR* CommandLineToArgvW(const wchar_t*, int* count) {
  if (!commandline_ok) return nullptr;
  main_arguments[0] = const_cast<LPWSTR>(L"dalvikvm.exe");
  main_arguments[1] = const_cast<LPWSTR>(L"-cp");
  main_arguments[2] = const_cast<LPWSTR>(L"com.gesher.server.SystemServer");
  main_arguments[3] = const_cast<LPWSTR>(app_role ? L"android.app.GesherAppProcess" : L"com.gesher.server.SystemServer");
  main_arguments[4] = const_cast<LPWSTR>(L"com.gesher.server.SystemServer");
  *count = 5; ++argv_allocations; return main_arguments;
}
void* LocalFree(void* data) {
  check(data == main_arguments, "Free the parsed native command line");
  --argv_allocations; return nullptr;
}
int main(int argc, char** argv) {
  app_role = argc > 1 && std::strcmp(argv[1], "app") == 0;
  commandline_ok = !(argc > 1 && std::strcmp(argv[1], "unknown") == 0);
  if (app_role || !commandline_ok) {
    reset();
    check(!gesher::devsvc::DeviceAdminServer(), "App extras/classpath or missing command line cannot authorize locking");
    app_role = false; commandline_ok = true;
    check(!gesher::devsvc::DeviceAdminServer(), "The captured role cannot change after native registration");
    check(!gesher::devsvc::LockWorkstation(), "Direct host-lock call from an application is denied");
    check(lock_calls == 0 && session_queries == 0 && sleep_calls == 0,
          "Unauthorized call never reaches session or lock operations");
    check(argv_allocations == 0, "Role detection frees its command line");
    std::printf("Device administrator native role denied: %d checks\n", checks);
    return 0;
  }
  check(gesher::devsvc::DeviceAdminServer(), "Exact SystemServer main authorizes its backend");
  check(argv_allocations == 0, "Server role capture frees its command line");
  const wchar_t* options_only[] = {L"dalvikvm.exe", L"-cp", L"com.gesher.server.SystemServer"};
  check(!GesherDeviceAdminServerMain(3, options_only), "Classpath is never a main class");
  const wchar_t* missing_cp[] = {L"dalvikvm.exe", L"-cp"};
  check(!GesherDeviceAdminServerMain(2, missing_cp), "Truncated classpath fails closed");
  const wchar_t* main[] = {L"dalvikvm.exe", L"-Xint", L"-classpath", L"classes.dex", L"com.gesher.server.SystemServer"};
  check(GesherDeviceAdminServerMain(5, main), "ART options and classpath precede server main");
  const wchar_t* lookalike[] = {L"dalvikvm.exe", L"com.gesher.server.SystemServerExtra"};
  check(!GesherDeviceAdminServerMain(2, lookalike), "A lookalike main class is not trusted");
  const wchar_t* interleaved[] = {L"dalvikvm.exe", L"-cp", L"-XXlib:libart.dll",
      L"com.gesher.server.SystemServer", L"android.app.GesherAppProcess"};
  check(!GesherDeviceAdminServerMain(5, interleaved), "An interleaved library option does not consume the classpath value");
  const wchar_t* repeated_cp[] = {L"dalvikvm.exe", L"-cp", L"-classpath",
      L"com.gesher.server.SystemServer", L"android.app.GesherAppProcess"};
  check(!GesherDeviceAdminServerMain(5, repeated_cp), "Repeated classpath options cannot turn their operand into a role");
  check(!GesherDeviceAdminServerMain(0, nullptr), "Missing native argument list fails closed");
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
  reset(); lock_flags = WTS_SESSIONSTATE_LOCK;
  check(gesher::devsvc::LockWorkstation(), "Already locked session satisfies lock request");
  check(lock_calls == 0 && sleep_calls == 0, "Already locked session is not submitted again");
  reset(); lock_submit_ok = false;
  check(!gesher::devsvc::LockWorkstation(), "Rejected Windows lock submission is not success");
  check(lock_calls == 1 && sleep_calls == 0, "Rejected submission does not poll");
  reset();
  check(!gesher::devsvc::LockWorkstation(), "Submitted request without locked state is not success");
  check(lock_calls == 1 && sleep_calls == 150 && ticks == 3000,
        "Unconfirmed submission stops at the three-second deadline");
  reset(); query_ok = false;
  check(!gesher::devsvc::LockWorkstation(), "Unknown session state cannot confirm a submitted lock");
  check(sleep_calls == 150 && ticks == 3000, "Unknown state polling is bounded");
  reset(); confirm_after_sleeps = 2;
  check(gesher::devsvc::LockWorkstation(), "Asynchronous lock succeeds after real locked observation");
  check(lock_calls == 1 && sleep_calls == 2 && ticks == 40, "Confirmed lock returns without the full timeout");
  reset(); confirm_after_sleeps = 0;
  check(gesher::devsvc::LockWorkstation(), "Immediately observed lock succeeds");
  check(lock_calls == 1 && sleep_calls == 0, "Immediate confirmation needs no sleep");
  reset(); std::printf("Clipboard native boundary all passed: %d checks\n", checks);
}
