/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Included after permission_dialog.inc (PermissionControl, host theme). The separate
 * background-access choice of Android 16's PermissionController (BackgroundGrantBehavior):
 * a foreground Allow never grants background access. */

enum { BACKGROUND_PACKAGE = 1301, BACKGROUND_DETAILS = 1302, BACKGROUND_BRAND = 1303,
       BACKGROUND_HEADING = 1304, BACKGROUND_ALWAYS = 1305, BACKGROUND_FOREGROUND = 1306,
       BACKGROUND_DENY = 1307 };
// Keep these values in sync with PermissionRequests.java (DISMISSED, DENIED,
// FOREGROUND_ONLY, ALWAYS). Cancellation is not a user decision.
enum { BACKGROUND_DISMISSED = 0, BACKGROUND_DENIED = 1, BACKGROUND_FOREGROUND_ONLY = 3,
       BACKGROUND_ALWAYS_ALLOWED = 4 };

typedef struct {
  enum GesherUiLanguage language;
  const WCHAR* package_name;
  const WCHAR* permissions;
  HFONT heading_font;
  HFONT brand_font;
  GesherHostTheme theme;
  GesherHostIcons icons;
  int decision;
} BackgroundDialogState;

static BOOL BackgroundCreateControls(HWND dialog, BackgroundDialogState* state) {
  GesherHostThemeInit(&state->theme, dialog);
  BOOL rtl = GesherUiRtl(state->language);
  DWORD reading = rtl ? WS_EX_RTLREADING : 0;
  DWORD alignment = rtl ? SS_RIGHT : SS_LEFT;
  HWND brand = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "brand"),
      alignment, BACKGROUND_BRAND, 16, 10, 218, 14);
  HWND heading = PermissionControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "background_permission_heading"),
      alignment, BACKGROUND_HEADING, 16, 28, 218, 24);
  HWND app = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "permission_app"),
      alignment, -1, 16, 58, 218, 14);
  // The authenticated package ID has its own LTR control, without bidi isolates.
  HWND package = PermissionControl(dialog, WS_EX_LTRREADING, L"EDIT", state->package_name,
      ES_READONLY | ES_AUTOHSCROLL | ES_LEFT | WS_BORDER | WS_TABSTOP, BACKGROUND_PACKAGE, 16, 74, 218, 18);
  HWND requested = PermissionControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "background_permission_requested"), alignment, -1, 16, 100, 218, 24);
  HWND details = PermissionControl(dialog, reading, L"EDIT", state->permissions,
      ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP
      | (rtl ? ES_RIGHT : ES_LEFT), BACKGROUND_DETAILS, 16, 126, 218, 30);
  // Three full-width choices, as on Android's permission page. The safe default keeps
  // background access off without changing the foreground decision.
  HWND foreground = PermissionControl(dialog, 0, L"BUTTON",
      GesherUiText(state->language, "background_permission_foreground"),
      BS_OWNERDRAW | WS_TABSTOP, BACKGROUND_FOREGROUND, 16, 166, 218, 20);
  HWND always = PermissionControl(dialog, 0, L"BUTTON",
      GesherUiText(state->language, "background_permission_always"),
      BS_OWNERDRAW | WS_TABSTOP, BACKGROUND_ALWAYS, 16, 190, 218, 20);
  HWND deny = PermissionControl(dialog, 0, L"BUTTON",
      GesherUiText(state->language, "background_permission_deny"),
      BS_OWNERDRAW | WS_TABSTOP, BACKGROUND_DENY, 16, 214, 218, 20);
  if (!brand || !heading || !app || !package || !requested || !details || !foreground || !always || !deny) {
    return FALSE;
  }
  GesherHostSetFont(heading, &state->heading_font, 145, FW_SEMIBOLD);
  GesherHostSetFont(brand, &state->brand_font, 110, FW_BOLD);
  return TRUE;
}

static INT_PTR CALLBACK BackgroundDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  BackgroundDialogState* state = (BackgroundDialogState*)GetWindowLongPtrW(dialog, DWLP_USER);
  if (message == WM_INITDIALOG) {
    state = (BackgroundDialogState*)lparam;
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
    if (previous == 0 && GetLastError() != 0) { EndDialog(dialog, IDCANCEL); return FALSE; }
    if (!BackgroundCreateControls(dialog, state)) {
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    HWND foreground = GetDlgItem(dialog, BACKGROUND_FOREGROUND);
    SendMessageW(dialog, DM_SETDEFID, BACKGROUND_FOREGROUND, 0);
    GesherHostSetIcons(dialog, &state->icons);
    // Request foreground once; never keep the consent window permanently topmost.
    if (!SetForegroundWindow(dialog)) {
      FLASHWINFO flash = {sizeof(flash), dialog, FLASHW_TRAY, 3, 0};
      FlashWindowEx(&flash);
    }
    SetFocus(foreground);
    return FALSE;
  }
  if (state == NULL) return FALSE;
  if (message == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, BACKGROUND_HEADING), &state->heading_font, 145, FW_SEMIBOLD);
    GesherHostSetFont(GetDlgItem(dialog, BACKGROUND_BRAND), &state->brand_font, 110, FW_BOLD);
    return TRUE;
  }
  if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
  if (message == WM_COMMAND) {
    int command = LOWORD(wparam);
    if (command == BACKGROUND_ALWAYS || command == BACKGROUND_FOREGROUND
        || command == BACKGROUND_DENY || command == IDCANCEL) {
      state->decision = command == BACKGROUND_ALWAYS ? BACKGROUND_ALWAYS_ALLOWED
          : command == BACKGROUND_FOREGROUND ? BACKGROUND_FOREGROUND_ONLY
          : command == BACKGROUND_DENY ? BACKGROUND_DENIED : BACKGROUND_DISMISSED;
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
  return GesherHostThemeEvent(dialog, &state->theme, message, wparam, lparam, BACKGROUND_BRAND);
}

static int BackgroundPermissionDialog(enum GesherUiLanguage language, const WCHAR* package_name,
        const WCHAR* permissions) {
  BackgroundDialogState state = {0};
  state.language = language;
  state.package_name = package_name;
  state.permissions = permissions;
  union { DWORD alignment; WORD words[128]; } buffer = {0};
  DLGTEMPLATE* dialog = (DLGTEMPLATE*)buffer.words;
  dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  dialog->dwExtendedStyle = GesherUiRtl(language) ? WS_EX_RTLREADING : 0;
  dialog->cx = 250;
  dialog->cy = 246;
  WORD* at = (WORD*)(dialog + 1);
  *at++ = 0; // No menu.
  *at++ = 0; // Default dialog class.
  const WCHAR* title = GesherUiText(language, "background_permission_title");
  SIZE_T title_chars = wcslen(title) + 1;
  const WCHAR font[] = L"Segoe UI";
  if ((SIZE_T)(at - buffer.words) + title_chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) return BACKGROUND_DISMISSED;
  for (SIZE_T i = 0; i < title_chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  GesherHostLoadIcons(&state.icons);
  INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, BackgroundDialogProc,
      (LPARAM)&state);
  GesherHostFreeIcons(&state.icons);
  return result == -1 ? BACKGROUND_DISMISSED : state.decision;
}
