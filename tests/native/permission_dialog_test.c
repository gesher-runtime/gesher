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
static BOOL prompt_confirmation;
static int prompt_action;
static const WCHAR* prompt_message = L"com.example.app\r\nandroid.permission.CAMERA";
static BOOL prompt_scrolls;
static BOOL prompt_truncated;
static WCHAR prompt_text[40002];
static WCHAR huge_message[40001];

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
  Check((GetWindowLongW(deny, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW,
      "Deny button uses the shared modern host presentation");
  Check((GetWindowLongW(allow, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW,
      "Allow button uses the shared modern host presentation");
  /* A single-line EDIT drops WS_BORDER from its style and draws the frame inside
   * its client area; the frame shows up as an inset of the formatting rectangle. */
  RECT package_text = {0};
  SendMessageW(package, EM_GETRECT, 0, (LPARAM)&package_text);
  Check(package_text.top >= GetSystemMetrics(SM_CYBORDER),
      "package identifier has a bounded read-only surface");
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

/* The same UI component handles the system-server's account-token consent and
 * the shell's error/crash messages. Verify those real paths, not a duplicate UI. */
static void CALLBACK InspectHostPrompt(HWND dialog, UINT message, UINT_PTR timer, DWORD time) {
  (void)message; (void)time;
  KillTimer(dialog, timer);
  if (phase == 1) {
    Check(GetFocus() == GetDlgItem(dialog, IDYES), "Tab from default Deny reaches host Allow");
    PostMessageW(GetFocus(), WM_KEYDOWN, VK_RETURN, 0);
    return;
  }
  HWND text = GetDlgItem(dialog, GESHER_HOST_MESSAGE_TEXT);
  HWND accept = GetDlgItem(dialog, prompt_confirmation ? IDYES : IDOK);
  HWND deny = GetDlgItem(dialog, IDCANCEL);
  Check(text != NULL && accept != NULL, "host prompt has readable text and an action");
  Check((deny != NULL) == prompt_confirmation, "only confirmations provide a deny action");
  Check(LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) ==
      (prompt_confirmation ? IDCANCEL : IDOK), "prompt default is safe");
  Check(GetFocus() == (prompt_confirmation ? deny : accept),
      "initial prompt focus follows the safe default");
  Check((GetWindowLongW(accept, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW,
      "host prompt uses the shared modern action");
  WCHAR klass[16] = {0};
  GetClassNameW(text, klass, 16);
  if (prompt_scrolls) {
    Check(lstrcmpiW(klass, L"Edit") == 0
        && (GetWindowLongW(text, GWL_STYLE) & (ES_READONLY | ES_MULTILINE | WS_VSCROLL))
        == (ES_READONLY | ES_MULTILINE | WS_VSCROLL), "long host message is scrollable and read-only");
  } else {
    // Short text is plain dialog text (announced with the dialog), never a mnemonic.
    Check(lstrcmpiW(klass, L"Static") == 0
        && (GetWindowLongW(text, GWL_STYLE) & SS_NOPREFIX) != 0,
        "short host message is static dialog text");
  }
  Check(((GetWindowLongW(dialog, GWL_EXSTYLE) & WS_EX_RTLREADING) != 0)
      == GesherUiRtl(language), "host prompt title reading direction follows locale");
  int length = GetWindowTextW(text, prompt_text, 40002);
  if (prompt_truncated) {
    Check(length == 32001 && wcsncmp(prompt_text, prompt_message, 32000) == 0
        && prompt_text[32000] == 0x2026, "oversized host message is shown truncated, not dropped");
  } else {
    Check(wcscmp(prompt_text, prompt_message) == 0, "host prompt preserves its multiline content");
  }
  if (prompt_action == ALLOW) {
    SendMessageW(accept, BM_CLICK, 0, 0);
  } else if (prompt_action == DENY) {
    SendMessageW(deny, BM_CLICK, 0, 0);
  } else if (prompt_action == CLOSE) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
  } else if (prompt_action == ESCAPE || prompt_action == ENTER) {
    PostMessageW(GetFocus(), WM_KEYDOWN, prompt_action == ESCAPE ? VK_ESCAPE : VK_RETURN, 0);
  } else if (prompt_action == TAB_ALLOW) {
    phase = 1;
    PostMessageW(GetFocus(), WM_KEYDOWN, VK_TAB, 0);
    SetTimer(dialog, 1, 20, InspectHostPrompt);
  }
}

static LRESULT CALLBACK AfterInit(int code, WPARAM wparam, LPARAM lparam) {
  if (code >= 0) {
    CWPRETSTRUCT* call = (CWPRETSTRUCT*)lparam;
    if (call->message == WM_INITDIALOG) {
      if (GetDlgItem(call->hwnd, PERMISSION_DETAILS)) SetTimer(call->hwnd, 1, 20, Inspect);
      else if (GetDlgItem(call->hwnd, GESHER_HOST_MESSAGE_TEXT))
        SetTimer(call->hwnd, 1, 20, InspectHostPrompt);
    }
  }
  return CallNextHookEx(NULL, code, wparam, lparam);
}

static DWORD WINAPI Watchdog(void* completed) {
  if (WaitForSingleObject((HANDLE)completed, 120000) != WAIT_OBJECT_0) {
    fprintf(stderr, "FAIL: permission dialog test exceeded 120 seconds\n");
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
  // Account token consent is always default-No; Enter, Escape and close deny.
  for (int locale = 0; locale < 2; ++locale) {
    language = locale ? GESHER_UI_HE : GESHER_UI_EN;
    prompt_confirmation = TRUE;
    for (prompt_action = DENY; prompt_action <= TAB_ALLOW; ++prompt_action) {
      phase = 0;
      BOOL accepted = GesherHostPrompt(GesherUiText(language, "account_title"),
          L"com.example.app\r\nandroid.permission.CAMERA",
          GesherUiText(language, "permission_allow"),
          GesherUiText(language, "permission_deny"),
          GesherUiRtl(language), NULL);
      Check(accepted == (prompt_action == ALLOW || prompt_action == TAB_ALLOW),
          "only an explicit account-token Allow grants access");
    }
    // Launcher notices are informational: OK closes them, and X cancels safely.
    prompt_confirmation = FALSE;
    for (prompt_action = ALLOW; prompt_action <= CLOSE; ++prompt_action) {
      phase = 0;
      BOOL accepted = GesherHostPrompt(GesherUiText(language, "brand"),
          L"com.example.app\r\nandroid.permission.CAMERA",
          GesherUiText(language, "cp_dialog_ok"), NULL,
          GesherUiRtl(language), NULL);
      Check(accepted == (prompt_action == ALLOW), "information popup exits safely");
    }
  }
  // A long launch failure scrolls; one beyond the EDIT limit is still presented.
  prompt_confirmation = FALSE;
  prompt_action = ALLOW;
  phase = 0;
  prompt_scrolls = TRUE;
  prompt_message = permissions;
  Check(GesherHostPrompt(GesherUiText(language, "brand"), prompt_message,
      GesherUiText(language, "cp_dialog_ok"), NULL, GesherUiRtl(language), NULL),
      "long information popup is presented");
  for (int i = 0; i < 40000; ++i) huge_message[i] = (i % 8 == 7) ? L' ' : L'a' + (i % 7);
  huge_message[40000] = 0;
  prompt_message = huge_message;
  prompt_truncated = TRUE;
  Check(GesherHostPrompt(GesherUiText(language, "brand"), prompt_message,
      GesherUiText(language, "cp_dialog_ok"), NULL, GesherUiRtl(language), NULL),
      "oversized information popup is presented");
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
