/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Owns one never-switched desktop and a kill-on-close job of synthetic fixture VMs. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>

typedef struct { DWORD pid; int visible; int matching; RECT bounds; BOOL bridge; } Search;
static BOOL CALLBACK FindWindowForPid(HWND window, LPARAM parameter) {
  Search* search = (Search*)parameter;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != search->pid) return TRUE;
  WCHAR title[128];
  if (!GetWindowTextW(window, title, 128)) return TRUE;
  if (search->bridge) {
    if (wcscmp(title, L"Gesher empty bridge fixture") && wcscmp(title, L"Gesher bridge dialog fixture")) return TRUE;
  } else if (wcscmp(title, L"Gesher window promotion fixture") && wcscmp(title, L"Gesher unapproved promotion fixture")) return TRUE;
  ++search->matching;
  RECT bounds, client, screen = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0}, overlap;
  screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (IsWindowVisible(window) && GetWindowRect(window, &bounds) && GetClientRect(window, &client)
      && client.right >= (search->bridge ? 1 : 80) && client.bottom >= (search->bridge ? 1 : 80)
      && IntersectRect(&overlap, &bounds, &screen)) {
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
typedef BOOL (WINAPI* DeriveCapabilities)(LPCWSTR, PSID**, DWORD*, PSID**, DWORD*);

/* This desktop belongs exclusively to the fixture. Give Gesher's trusted
   runtime capability the same five desktop rights used by its launcher.
   Preserve the integrity label. The controller never switches the desktop
   or changes the ACL of the existing desktop or window station. */
static BOOL GrantOwnedDesktop(HDESK desktop) {
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  DeriveCapabilities derive = kernelbase ?
      (DeriveCapabilities)(void*)GetProcAddress(kernelbase, "DeriveCapabilitySidsFromName") : NULL;
  if (derive == NULL) return FALSE;
  PSID *groups = NULL, *sids = NULL;
  DWORD groupCount = 0, sidCount = 0;
  if (!derive(L"gesherRuntimeFiles", &groups, &groupCount, &sids, &sidCount)) return FALSE;
  BOOL ok = FALSE;
  PSECURITY_DESCRIPTOR original = NULL;
  PACL updated = NULL, existing = NULL;
  SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION;
  DWORD needed = 0, originalSize = 0;
  BOOL present = FALSE, defaulted = FALSE;
  if (sidCount == 1 && IsValidSid(sids[0]) &&
      !GetUserObjectSecurity(desktop, &info, NULL, 0, &needed) &&
      GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed <= 65536) {
    originalSize = needed;
    original = HeapAlloc(GetProcessHeap(), 0, originalSize);
  }
  if (original != NULL && GetUserObjectSecurity(desktop, &info, original, originalSize, &needed) &&
      GetSecurityDescriptorDacl(original, &present, &existing, &defaulted) && present && existing) {
    EXPLICIT_ACCESSW entry = {0};
    entry.grfAccessPermissions = DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE |
        DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.ptstrName = (LPWSTR)sids[0];
    SECURITY_DESCRIPTOR descriptor;
    DWORD aclError = SetEntriesInAclW(1, &entry, existing, &updated);
    if (aclError == ERROR_SUCCESS) {
      ok = InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
          SetSecurityDescriptorDacl(&descriptor, TRUE, updated, FALSE) &&
          SetUserObjectSecurity(desktop, &info, &descriptor);
    } else SetLastError(aclError);
  }
  DWORD error = GetLastError();
  if (original != NULL) HeapFree(GetProcessHeap(), 0, original);
  if (updated != NULL) LocalFree(updated);
  for (DWORD i = 0; i < groupCount; ++i) LocalFree(groups[i]);
  for (DWORD i = 0; i < sidCount; ++i) LocalFree(sids[i]);
  if (groups) LocalFree(groups);
  if (sids) LocalFree(sids);
  SetLastError(error);
  return ok;
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
  BOOL bridge = wcscmp(argv[5], L"empty-bridge") == 0;
  CHECK(positive || bridge || wcscmp(argv[5], L"explicit-headless") == 0);
  commandBytes = ReadSmall(argv[1], 65536);
  CHECK(commandBytes != NULL);
  WCHAR* command = (WCHAR*)commandBytes;
  if (command[0] == 0xfeff) ++command;
  WCHAR name[80];
  WCHAR suffix[40];
  GUID unique;
  CHECK(SUCCEEDED(CoCreateGuid(&unique)) && StringFromGUID2(&unique, suffix, 40) > 0);
  CHECK(swprintf(name, 80, L"GesherPromotionTest-%ls", suffix) > 0);
  SetLastError(ERROR_SUCCESS);
  desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
  CHECK(desktop != NULL && GetLastError() != ERROR_ALREADY_EXISTS && original != NULL);
  CHECK(GrantOwnedDesktop(desktop) && SetThreadDesktop(desktop));
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
  Search search = {targetPid, 0, 0, {0}, bridge};
  deadline = GetTickCount64() + ((positive || bridge) ? 10000 : 2500);
  do {
    search.visible = search.matching = 0;
    CHECK(EnumDesktopWindows(desktop, FindWindowForPid, (LPARAM)&search));
    CHECK(WaitForSingleObject(target, 0) == WAIT_TIMEOUT);
    if ((positive || bridge) && search.visible > 0) break;
    if (!positive && !bridge) CHECK(search.visible == 0);
    Sleep(25);
  } while (GetTickCount64() < deadline);
  printf("window promotion observed: visible=%d matching=%d targetJob=true\n", search.visible, search.matching);
  fflush(stdout);
  CHECK(positive || bridge ? search.visible == 1 : search.visible == 0);
  if (bridge) CHECK(search.matching == 2);
  // The worker polls these markers while this controller is their sole writer.
  HANDLE result = CreateFileW(argv[4], GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  CHECK(result != INVALID_HANDLE_VALUE);
  const char pass[] = "PASS\n";
  DWORD written = 0;
  BOOL wrote = WriteFile(result, pass, sizeof(pass)-1, &written, NULL);
  CloseHandle(result);
  CHECK(wrote && written == sizeof(pass)-1);
  if (bridge) {
    BOOL contentReady = FALSE;
    deadline = GetTickCount64() + 25000;
    do {
      char* state = ReadSmall(argv[3], 128);
      if (state != NULL) {
        unsigned long number = 0;
        contentReady = sscanf(state, "CONTENT %lu", &number) == 1 && (DWORD)number == targetPid;
        HeapFree(GetProcessHeap(), 0, state);
      }
      if (contentReady || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
      Sleep(20);
    } while (GetTickCount64() < deadline);
    CHECK(contentReady);
    deadline = GetTickCount64() + 10000;
    do {
      search.visible = search.matching = 0;
      CHECK(EnumDesktopWindows(desktop, FindWindowForPid, (LPARAM)&search));
      CHECK(WaitForSingleObject(target, 0) == WAIT_TIMEOUT);
      if (search.visible == 2) break;
      Sleep(25);
    } while (GetTickCount64() < deadline);
    CHECK(search.visible == 2 && search.matching == 2);
    result = CreateFileW(argv[4], FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(result != INVALID_HANDLE_VALUE);
    const char contentPass[] = "CONTENT_PASS\n";
    wrote = WriteFile(result, contentPass, sizeof(contentPass)-1, &written, NULL);
    CloseHandle(result);
    CHECK(wrote && written == sizeof(contentPass)-1);
  }
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
    result = CreateFileW(argv[4], FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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
      bridge ? "empty-bridge" : positive ? "positive" : "explicit-headless", checks, search.visible, search.matching,
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
