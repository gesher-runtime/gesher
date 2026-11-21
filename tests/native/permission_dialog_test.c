/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Exercises only synthetic dialogs on a private desktop that is never shown. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../../compat/locale/ui_locale.h"
#include "../../compat/credentials/permission_dialog.inc"

enum { DENY, ALLOW, CLOSE, ENTER, ESCAPE, TAB_ALLOW };
static int action;
static int checks;
static int failures;
static int phase;
static enum GesherUiLanguage language;
static WCHAR permissions[8193];

static void Check(BOOL condition, const char* message) {
  ++checks;
  if (!condition) { ++failures; fprintf(stderr, "FAIL: %s\n", message); }
}

static void CALLBACK Inspect(HWND dialog, UINT message, UINT_PTR timer, DWORD time) {
  (void)message; (void)time;
  KillTimer(dialog, timer);
  if (phase == 1) {
    Check(GetFocus() == GetDlgItem(dialog, IDYES), "Tab reaches Allow from default Deny");
    PostMessageW(GetFocus(), WM_KEYDOWN, VK_RETURN, 0);
    return;
  }
  HWND package = GetDlgItem(dialog, PERMISSION_PACKAGE);
  HWND details = GetDlgItem(dialog, PERMISSION_DETAILS);
  HWND deny = GetDlgItem(dialog, IDCANCEL);
  HWND allow = GetDlgItem(dialog, IDYES);
  Check(package && details && deny && allow, "all interactive controls exist");
  Check(GetFocus() == deny, "initial focus is Deny");
  Check(LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDCANCEL, "default action is Deny");
  Check((GetWindowLongW(package, GWL_EXSTYLE) & (WS_EX_RTLREADING | WS_EX_LAYOUTRTL)) == 0,
      "package identifier stays LTR in both locales");
  Check((GetWindowLongW(package, GWL_STYLE) & ES_READONLY) != 0, "package is read-only");
  Check((GetWindowLongW(details, GWL_EXSTYLE) & WS_EX_RTLREADING) ==
      (GesherUiRtl(language) ? WS_EX_RTLREADING : 0), "permission reading direction follows locale");
  Check((GetWindowLongW(details, GWL_STYLE) & (ES_READONLY | ES_MULTILINE | WS_VSCROLL)) ==
      (ES_READONLY | ES_MULTILINE | WS_VSCROLL), "permissions are read-only multiline with scrollbar");
  WCHAR value[8193];
  GetWindowTextW(package, value, 8193);
  Check(wcscmp(value, L"com.ideomobile.mercantile") == 0, "exact package identifier, no bidi isolates");
  GetWindowTextW(details, value, 8193);
  Check(wcscmp(value, permissions) == 0, "long permission text is preserved without truncation");
  GetWindowTextW(deny, value, 8193);
  Check(wcscmp(value, GesherUiText(language, "permission_deny")) == 0, "localized Deny label");
  GetWindowTextW(allow, value, 8193);
  Check(wcscmp(value, GesherUiText(language, "permission_allow")) == 0, "localized Allow label");
  SendMessageW(details, EM_LINESCROLL, 0, 60);
  Check(SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0) > 0, "long request scrolls vertically");
  RECT client, bounds;
  GetClientRect(dialog, &client);
  GetWindowRect(allow, &bounds);
  MapWindowPoints(NULL, dialog, (POINT*)&bounds, 2);
  Check(bounds.right <= client.right && bounds.bottom <= client.bottom, "Allow fits inside dialog");
  if (action == DENY || action == ALLOW) {
    SendMessageW(GetDlgItem(dialog, action == ALLOW ? IDYES : IDCANCEL), BM_CLICK, 0, 0);
  } else if (action == CLOSE) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
  } else if (action == TAB_ALLOW) {
    phase = 1;
    PostMessageW(GetFocus(), WM_KEYDOWN, VK_TAB, 0);
    SetTimer(dialog, 1, 20, Inspect);
  } else {
    PostMessageW(GetFocus(), WM_KEYDOWN, action == ESCAPE ? VK_ESCAPE : VK_RETURN, 0);
  }
}

static LRESULT CALLBACK AfterInit(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0) {
    CWPRETSTRUCT* call = (CWPRETSTRUCT*)lparam;
    if (call->message == WM_INITDIALOG && GetDlgItem(call->hwnd, PERMISSION_DETAILS)) {
      SetTimer(call->hwnd, 1, 20, Inspect);
    }
  }
  return CallNextHookEx(NULL, code, wparam, lparam);
}

static DWORD WINAPI Watchdog(void* completed) {
  if (WaitForSingleObject((HANDLE)completed, 45000) != WAIT_OBJECT_0) {
    fprintf(stderr, "FAIL: permission dialog test exceeded 45 seconds\n");
    TerminateProcess(GetCurrentProcess(), 4);
  }
  return 0;
}

int main(void) {
  HANDLE completed = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!completed) return 2;
  HANDLE watchdog = CreateThread(NULL, 0, Watchdog, completed, 0, NULL);
  if (!watchdog) { CloseHandle(completed); return 2; }
  WCHAR name[80];
  swprintf(name, 80, L"gesher-permission-qa-%lu", GetCurrentProcessId());
  HDESK original = GetThreadDesktop(GetCurrentThreadId());
  HDESK desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
  if (!desktop || !SetThreadDesktop(desktop)) return 2;
  HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROCRET, AfterInit, NULL, GetCurrentThreadId());
  if (!hook) return 3;
  for (int locale = 0; locale < 2; ++locale) {
    language = locale ? GESHER_UI_HE : GESHER_UI_EN;
    permissions[0] = 0;
    for (int row = 0; row < 90; ++row) {
      wcscat(permissions, locale
          ? L"\u05e6\u05d9\u05dc\u05d5\u05dd \u05ea\u05de\u05d5\u05e0\u05d5\u05ea \u202a(android.permission.CAMERA)\u202c\r\n"
          : L"Take photos \u202a(android.permission.CAMERA)\u202c\r\n");
    }
    for (action = DENY; action <= TAB_ALLOW; ++action) {
      phase = 0;
      BOOL allowed = PermissionDialog(language, L"com.ideomobile.mercantile", permissions);
      Check(allowed == (action == ALLOW || action == TAB_ALLOW), "only explicit Allow grants consent");
      printf("scenario: locale=%s action=%d result=%d\n", locale ? "he" : "en", action, allowed);
    }
  }
  UnhookWindowsHookEx(hook);
  SetThreadDesktop(original);
  CloseDesktop(desktop);
  SetEvent(completed);
  WaitForSingleObject(watchdog, 1000);
  CloseHandle(watchdog);
  CloseHandle(completed);
  printf("permission_dialog_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
