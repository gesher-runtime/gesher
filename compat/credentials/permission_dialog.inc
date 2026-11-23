/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Included by account_crypto.c. Uses ordinary accessible Windows controls. */
#include "../windows/host_dialog_theme.h"

enum { PERMISSION_PACKAGE = 1101, PERMISSION_DETAILS = 1102, PERMISSION_BRAND = 1103,
       PERMISSION_HEADING = 1104 };
// Keep these values in sync with PermissionRequests.java. Cancellation is not a user denial.
enum { PERMISSION_DISMISSED = 0, PERMISSION_DENIED = 1, PERMISSION_ALLOWED = 2 };

typedef struct {
  enum GesherUiLanguage language;
  const WCHAR* package_name;
  const WCHAR* permissions;
  HFONT heading_font;
  HFONT brand_font;
  GesherHostTheme theme;
  GesherHostIcons icons;
  int decision;
} PermissionDialogState;

static HWND PermissionControl(HWND dialog, DWORD extra, const WCHAR* klass,
        const WCHAR* text, DWORD style, int id, int x, int y, int width, int height) {
  RECT bounds = {x, y, x + width, y + height};
  if (!MapDialogRect(dialog, &bounds)) return NULL;
  HWND control = CreateWindowExW(extra, klass, text, WS_CHILD | WS_VISIBLE | style,
      bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
      dialog, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (control != NULL) {
    SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
  }
  return control;
}

static INT_PTR CALLBACK PermissionDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  PermissionDialogState* state = (PermissionDialogState*)GetWindowLongPtrW(dialog, DWLP_USER);
  if (message == WM_INITDIALOG) {
    state = (PermissionDialogState*)lparam;
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
    if (previous == 0 && GetLastError() != 0) { EndDialog(dialog, IDCANCEL); return FALSE; }
    GesherHostThemeInit(&state->theme, dialog);
    BOOL rtl = GesherUiRtl(state->language);
    DWORD reading = rtl ? WS_EX_RTLREADING : 0;
    DWORD alignment = rtl ? SS_RIGHT : SS_LEFT;
    HWND brand = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "brand"),
        alignment, PERMISSION_BRAND, 16, 10, 218, 14);
    HWND heading = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "permission_heading"),
        alignment, PERMISSION_HEADING, 16, 28, 218, 24);
    HWND app = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "permission_app"),
        alignment, -1, 16, 58, 218, 14);
    // The authenticated package ID has its own LTR control, without bidi isolates.
    HWND package = PermissionControl(dialog, WS_EX_LTRREADING, L"EDIT", state->package_name,
        ES_READONLY | ES_AUTOHSCROLL | ES_LEFT | WS_BORDER | WS_TABSTOP, PERMISSION_PACKAGE, 16, 74, 218, 18);
    HWND requested = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "permission_requested"),
        alignment, -1, 16, 100, 218, 16);
    // Long requests stay readable and selectable instead of overflowing the screen.
    HWND details = PermissionControl(dialog, reading, L"EDIT", state->permissions,
        ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP
        | (rtl ? ES_RIGHT : ES_LEFT), PERMISSION_DETAILS, 16, 118, 218, 36);
    HWND deny = PermissionControl(dialog, 0, L"BUTTON", GesherUiText(state->language, "permission_deny"),
        BS_OWNERDRAW | WS_TABSTOP, IDNO, 76, 166, 76, 22);
    HWND allow = PermissionControl(dialog, 0, L"BUTTON", GesherUiText(state->language, "permission_allow"),
        BS_OWNERDRAW | WS_TABSTOP, IDYES, 160, 166, 74, 22);
    if (!brand || !heading || !app || !package || !requested || !details || !deny || !allow) {
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    GesherHostSetFont(heading, &state->heading_font, 145, FW_SEMIBOLD);
    GesherHostSetFont(brand, &state->brand_font, 110, FW_BOLD);
    SendMessageW(dialog, DM_SETDEFID, IDNO, 0);
    GesherHostSetIcons(dialog, &state->icons);
    // Request foreground once; never keep the consent window permanently topmost.
    if (!SetForegroundWindow(dialog)) {
      FLASHWINFO flash = {sizeof(flash), dialog, FLASHW_TRAY, 3, 0};
      FlashWindowEx(&flash);
    }
    SetFocus(deny);
    return FALSE;
  }
  if (state == NULL) return FALSE;
  if (message == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, PERMISSION_HEADING), &state->heading_font, 145, FW_SEMIBOLD);
    GesherHostSetFont(GetDlgItem(dialog, PERMISSION_BRAND), &state->brand_font, 110, FW_BOLD);
    return TRUE;
  }
  if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
  if (message == WM_COMMAND) {
    int command = LOWORD(wparam);
    if (command == IDYES || command == IDNO || command == IDCANCEL) {
      state->decision = command == IDYES ? PERMISSION_ALLOWED
          : command == IDNO ? PERMISSION_DENIED : PERMISSION_DISMISSED;
      EndDialog(dialog, command);
      return TRUE;
    }
  }
  if (message == WM_DESTROY) {
    if (state->heading_font) DeleteObject(state->heading_font);
    if (state->brand_font) DeleteObject(state->brand_font);
    GesherHostThemeDispose(&state->theme);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  return GesherHostThemeEvent(dialog, &state->theme, message, wparam, lparam, PERMISSION_BRAND);
}

static int PermissionDialog(enum GesherUiLanguage language, const WCHAR* package_name, const WCHAR* permissions) {
  PermissionDialogState state = {0};
  state.language = language;
  state.package_name = package_name;
  state.permissions = permissions;
  union { DWORD alignment; WORD words[128]; } buffer = {0};
  DLGTEMPLATE* dialog = (DLGTEMPLATE*)buffer.words;
  dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  dialog->dwExtendedStyle = GesherUiRtl(language) ? WS_EX_RTLREADING : 0;
  dialog->cx = 250;
  dialog->cy = 200;
  WORD* at = (WORD*)(dialog + 1);
  *at++ = 0; // No menu.
  *at++ = 0; // Default dialog class.
  const WCHAR* title = GesherUiText(language, "permission_title");
  SIZE_T title_chars = wcslen(title) + 1;
  const WCHAR font[] = L"Segoe UI";
  if ((SIZE_T)(at - buffer.words) + title_chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) return PERMISSION_DISMISSED;
  for (SIZE_T i = 0; i < title_chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  GesherHostLoadIcons(&state.icons);
  INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, PermissionDialogProc, (LPARAM)&state);
  GesherHostFreeIcons(&state.icons);
  return result == -1 ? PERMISSION_DISMISSED : state.decision;
}
