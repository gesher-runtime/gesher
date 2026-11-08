/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Owns one never-switched desktop and a kill-on-close job of synthetic fixture VMs. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

typedef struct { DWORD pid; int visible; int matching; RECT bounds; } Search;
static BOOL CALLBACK FindWindowForPid(HWND window, LPARAM parameter) {
  Search* search = (Search*)parameter;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != search->pid) return TRUE;
  WCHAR title[128];
  if (!GetWindowTextW(window, title, 128)
      || (wcscmp(title, L"Gesher window promotion fixture") && wcscmp(title, L"Gesher unapproved promotion fixture"))) return TRUE;
  ++search->matching;
  RECT bounds, client, screen = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0}, overlap;
  screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (IsWindowVisible(window) && GetWindowRect(window, &bounds) && GetClientRect(window, &client)
      && client.right >= 80 && client.bottom >= 80 && IntersectRect(&overlap, &bounds, &screen)) {
    ++search->visible;
    search->bounds = bounds;
  }
  return TRUE;
}
static char* ReadSmall(const WCHAR* path, DWORD maximum) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 1 || size.QuadPart > maximum) { CloseHandle(file); return NULL; }
  char* text = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)size.QuadPart + 2);
  DWORD read = 0;
  if (text && (!ReadFile(file, text, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart)) {
    HeapFree(GetProcessHeap(), 0, text); text = NULL;
  }
  CloseHandle(file);
  return text;
}
int wmain(int argc, WCHAR** argv) {
  BOOL ok = FALSE;
  int checks = 0;
  HDESK desktop = NULL, original = GetThreadDesktop(GetCurrentThreadId());
  HANDLE job = NULL, target = NULL;
  PROCESS_INFORMATION process = {0};
  char* commandBytes = NULL;
  BOOL finished = FALSE;
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "window promotion controller failed line %d, Win32 %lu\n", __LINE__, GetLastError()); goto cleanup; } ++checks; } while (0)
  CHECK(argc == 6);
  BOOL positive = wcscmp(argv[5], L"positive") == 0;
  CHECK(positive || wcscmp(argv[5], L"explicit-headless") == 0);
  commandBytes = ReadSmall(argv[1], 65536);
  CHECK(commandBytes != NULL);
  WCHAR* command = (WCHAR*)commandBytes;
  if (command[0] == 0xfeff) ++command;
  WCHAR name[80];
  swprintf(name, 80, L"GesherPromotionTest-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
  desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
  CHECK(desktop != NULL && original != NULL && SetThreadDesktop(desktop));
  job = CreateJobObjectW(NULL, NULL);
  CHECK(job != NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  startup.lpDesktop = name;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
      NULL, argv[2], &startup, &process));
  CHECK(AssignProcessToJobObject(job, process.hProcess));
  CHECK(ResumeThread(process.hThread) != (DWORD)-1);
  DWORD targetPid = 0;
  ULONGLONG deadline = GetTickCount64() + 120000;
  do {
    char* state = ReadSmall(argv[3], 128);
    if (state != NULL) {
      unsigned long number = 0;
      if (sscanf(state, "CHECK %lu", &number) == 1) targetPid = (DWORD)number;
      HeapFree(GetProcessHeap(), 0, state);
    }
    if (targetPid || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
    Sleep(20);
  } while (GetTickCount64() < deadline);
  CHECK(targetPid > 0);
  target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, targetPid);
  CHECK(target != NULL);
  BOOL belongs = FALSE;
  CHECK(IsProcessInJob(target, job, &belongs) && belongs);
  Search search = {targetPid, 0, 0, {0}};
  deadline = GetTickCount64() + (positive ? 10000 : 2500);
  do {
    search.visible = search.matching = 0;
    CHECK(EnumDesktopWindows(desktop, FindWindowForPid, (LPARAM)&search));
    CHECK(WaitForSingleObject(target, 0) == WAIT_TIMEOUT);
    if (positive && search.visible > 0) break;
    if (!positive) CHECK(search.visible == 0);
    Sleep(25);
  } while (GetTickCount64() < deadline);
  printf("window promotion observed: visible=%d matching=%d targetJob=true\n", search.visible, search.matching);
  fflush(stdout);
  CHECK(positive ? search.visible == 1 : search.visible == 0);
  HANDLE result = CreateFileW(argv[4], GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  CHECK(result != INVALID_HANDLE_VALUE);
  const char pass[] = "PASS\n";
  DWORD written = 0;
  BOOL wrote = WriteFile(result, pass, sizeof(pass)-1, &written, NULL);
  CloseHandle(result);
  CHECK(wrote && written == sizeof(pass)-1);
  if (positive) {
    BOOL phaseReady = FALSE;
    deadline = GetTickCount64() + 35000;
    do {
      char* state = ReadSmall(argv[3], 128);
      if (state != NULL) {
        unsigned long number = 0;
        phaseReady = sscanf(state, "BACKGROUND %lu", &number) == 1 && (DWORD)number == targetPid;
        HeapFree(GetProcessHeap(), 0, state);
      }
      if (phaseReady || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
      Sleep(20);
    } while (GetTickCount64() < deadline);
    CHECK(phaseReady);
    deadline = GetTickCount64() + 2500;
    do {
      search.visible = search.matching = 0;
      CHECK(EnumDesktopWindows(desktop, FindWindowForPid, (LPARAM)&search));
      CHECK(WaitForSingleObject(target, 0) == WAIT_TIMEOUT);
      CHECK(search.visible == 0 && search.matching == 1);
      Sleep(25);
    } while (GetTickCount64() < deadline);
    printf("window promotion background observed: visible=%d matching=%d targetJob=true\n", search.visible, search.matching);
    fflush(stdout);
    result = CreateFileW(argv[4], FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(result != INVALID_HANDLE_VALUE);
    const char backgroundPass[] = "BACKGROUND_PASS\n";
    wrote = WriteFile(result, backgroundPass, sizeof(backgroundPass)-1, &written, NULL);
    CloseHandle(result);
    CHECK(wrote && written == sizeof(backgroundPass)-1);
  }
  CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
  finished = TRUE;
  DWORD code = 1;
  CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
  printf("window_promotion_test all passed: mode=%s checks=%d visible=%d matching=%d bounds=%ld,%ld,%ld,%ld\n",
      positive ? "positive" : "explicit-headless", checks, search.visible, search.matching,
      search.bounds.left, search.bounds.top, search.bounds.right, search.bounds.bottom);
  ok = TRUE;
cleanup:
  if (target != NULL) CloseHandle(target);
  if (process.hProcess != NULL) {
    if (!finished && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 90);
      WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
  if (job != NULL) CloseHandle(job);
  if (commandBytes != NULL) HeapFree(GetProcessHeap(), 0, commandBytes);
  if (original != NULL && !SetThreadDesktop(original)) ok = FALSE;
  if (desktop != NULL && !CloseDesktop(desktop)) ok = FALSE;
  return ok ? 0 : 1;
}
