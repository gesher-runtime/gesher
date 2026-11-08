/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Drives only an owned synthetic VM on a new, never-displayed Windows desktop. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

enum { CHOOSER_LIST = 1001, CHOOSER_ADD = 1002 };
typedef struct { DWORD pid; HWND dialog; } Search;

static BOOL CALLBACK FindDialog(HWND window, LPARAM parameter) {
  Search* search = (Search*)parameter;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != search->pid) return TRUE;
  WCHAR klass[32];
  if (GetClassNameW(window, klass, 32) && wcscmp(klass, L"#32770") == 0) {
    search->dialog = window;
    return FALSE;
  }
  return TRUE;
}

static BOOL Message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, DWORD_PTR* result) {
  return SendMessageTimeoutW(window, message, wparam, lparam,
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, result) != 0;
}

int wmain(int argc, WCHAR** argv) {
  int checks = 0;
  BOOL ok = FALSE;
  HDESK desktop = NULL;
  HDESK original = GetThreadDesktop(GetCurrentThreadId());
  PROCESS_INFORMATION process = {0};
  HANDLE job = NULL;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
  STARTUPINFOW startup = {0};
  HANDLE file = INVALID_HANDLE_VALUE;
  WCHAR* buffer = NULL;
  BOOL finished = FALSE;
  DWORD exit_code = 0;
  WCHAR name[80];
  LARGE_INTEGER length;
  DWORD read = 0;
  DWORD_PTR value = 0;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "account chooser fixture failed at line %d\n", __LINE__); goto cleanup; } ++checks; } while (0)
  CHECK(argc == 4);
  BOOL inputs = wcscmp(argv[2], L"inputs") == 0;
  BOOL client = wcscmp(argv[2], L"client") == 0;
  BOOL select = wcscmp(argv[2], L"select") == 0;
  BOOL add = wcscmp(argv[2], L"add") == 0;
  BOOL cancel = wcscmp(argv[2], L"cancel") == 0;
  BOOL noadd = wcscmp(argv[2], L"noadd") == 0;
  BOOL timer = wcscmp(argv[2], L"timer-cancel") == 0 || wcscmp(argv[2], L"timer-exception") == 0;
  CHECK(inputs || client || select || add || cancel || noadd || timer);
  file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  CHECK(file != INVALID_HANDLE_VALUE);
  CHECK(GetFileSizeEx(file, &length) && length.QuadPart >= 4 && length.QuadPart <= 65536
      && (length.QuadPart % sizeof(WCHAR)) == 0);
  buffer = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)length.QuadPart + sizeof(WCHAR));
  CHECK(buffer != NULL);
  CHECK(ReadFile(file, buffer, (DWORD)length.QuadPart, &read, NULL) && read == (DWORD)length.QuadPart);
  CloseHandle(file); file = INVALID_HANDLE_VALUE;
  WCHAR* command = buffer[0] == 0xfeff ? buffer + 1 : buffer;
  swprintf(name, 80, L"GesherChooserTest-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
  desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
  CHECK(desktop != NULL && original != NULL);
  CHECK(SetThreadDesktop(desktop));
  /* No SwitchDesktop call: the user's active desktop is never displayed or driven. */
  startup.cb = sizeof(startup);
  startup.lpDesktop = name;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  job = CreateJobObjectW(NULL, NULL);
  CHECK(job != NULL);
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
  CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
      NULL, argv[3], &startup, &process));
  CHECK(AssignProcessToJobObject(job, process.hProcess));
  CHECK(ResumeThread(process.hThread) != (DWORD)-1);
  if (!inputs && !client) {
    Search search = {process.dwProcessId, NULL};
    ULONGLONG deadline = GetTickCount64() + 10000;
    do {
      EnumDesktopWindows(desktop, FindDialog, (LPARAM)&search);
      if (search.dialog != NULL || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
      Sleep(10);
    } while (GetTickCount64() < deadline);
    CHECK(search.dialog != NULL);
    HWND dialog = search.dialog;
    HWND list = NULL;
    do {
      list = GetDlgItem(dialog, CHOOSER_LIST);
      if ((list != NULL && IsWindowVisible(dialog)) || !IsWindow(dialog)) break;
      Sleep(1);
    } while (GetTickCount64() < deadline);
    CHECK(list != NULL);
    HWND select_button = GetDlgItem(dialog, IDOK);
    HWND cancel_button = GetDlgItem(dialog, IDCANCEL);
    CHECK(select_button != NULL && cancel_button != NULL);
    WCHAR locale[32] = {0}, text[128] = {0};
    GetEnvironmentVariableW(L"GESHER_LOCALES", locale, 32);
    BOOL hebrew = wcscmp(locale, L"he-IL") == 0;
    CHECK(GetWindowTextW(dialog, text, 128) > 0);
    CHECK(wcscmp(text, hebrew ? L"\u05d2\u05e9\u05e8 \u2014 \u05d1\u05d7\u05d9\u05e8\u05ea \u05d7\u05e9\u05d1\u05d5\u05df" : L"Gesher accounts") == 0);
    CHECK(((GetWindowLongW(dialog, GWL_EXSTYLE) & WS_EX_RTLREADING) != 0) == hebrew);
    CHECK(GetWindowTextW(select_button, text, 128) > 0);
    CHECK(wcscmp(text, hebrew ? L"\u05d1\u05d7\u05d9\u05e8\u05d4" : L"Select") == 0);
    CHECK(GetWindowTextW(cancel_button, text, 128) > 0);
    CHECK(wcscmp(text, hebrew ? L"\u05d1\u05d9\u05d8\u05d5\u05dc" : L"Cancel") == 0);
    if (add) {
      CHECK(GetWindowTextW(GetDlgItem(dialog, CHOOSER_ADD), text, 128) > 0);
      CHECK(wcscmp(text, hebrew ? L"\u05d4\u05d5\u05e1\u05e4\u05ea \u05d7\u05e9\u05d1\u05d5\u05df" : L"Add account") == 0);
    }
    CHECK(!IsWindowEnabled(select_button));
    CHECK(Message(list, LB_GETCURSEL, 0, 0, &value) && (LRESULT)value == LB_ERR);
    CHECK(Message(dialog, DM_GETDEFID, 0, 0, &value) && LOWORD(value) == IDCANCEL);
    CHECK(Message(list, LB_GETCOUNT, 0, 0, &value) && value == (add ? 0 : 2));
    CHECK((GetDlgItem(dialog, CHOOSER_ADD) != NULL) == add);
    if (!timer) {
      CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)select_button, &value));
      CHECK(IsWindow(dialog) && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT);
      if (select) {
        CHECK(Message(list, LB_SETCURSEL, 1, 0, &value));
        CHECK(!IsWindowEnabled(select_button));
        CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)select_button, &value));
        CHECK(IsWindow(dialog));
        CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(CHOOSER_LIST, LBN_SELCHANGE), (LPARAM)list, &value));
        CHECK(IsWindowEnabled(select_button));
        CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)select_button, &value));
      } else if (add) {
        CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(CHOOSER_ADD, BN_CLICKED),
            (LPARAM)GetDlgItem(dialog, CHOOSER_ADD), &value));
      } else {
        if (noadd) {
          CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(CHOOSER_ADD, BN_CLICKED), 0, &value));
          CHECK(IsWindow(dialog));
        }
        CHECK(Message(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)cancel_button, &value));
      }
    }
  }
  CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
  finished = TRUE;
  CHECK(GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0);
  ok = TRUE;
cleanup:
  if (process.hProcess != NULL) {
    if (!finished && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 90);
      WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  if (job != NULL) CloseHandle(job);
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (buffer != NULL) { SecureZeroMemory(buffer, (SIZE_T)length.QuadPart + sizeof(WCHAR)); HeapFree(GetProcessHeap(), 0, buffer); }
  if (original != NULL && !SetThreadDesktop(original)) ok = FALSE;
  if (desktop != NULL && !CloseDesktop(desktop)) ok = FALSE;
  if (!ok) return 1;
  printf("account_chooser_test all passed: %d checks\n", checks);
  return 0;
}
