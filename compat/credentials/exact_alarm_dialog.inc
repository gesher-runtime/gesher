/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Native Settings presentation. Uses PermissionControl/PermissionFont from permission_dialog.inc. */
enum { EXACT_ALARM_PACKAGES = 1201, EXACT_ALARM_ALLOWED = 1202 };

typedef struct {
  enum GesherUiLanguage language;
  const WCHAR* const* packages;
  const unsigned char* allowed;
  int count;
  int result;
  HFONT heading_font;
} ExactAlarmDialogState;

static INT_PTR CALLBACK ExactAlarmDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  ExactAlarmDialogState* state = (ExactAlarmDialogState*)GetWindowLongPtrW(dialog, DWLP_USER);
  if (message == WM_INITDIALOG) {
    state = (ExactAlarmDialogState*)lparam;
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
    if (previous == 0 && GetLastError() != 0) { EndDialog(dialog, IDCANCEL); return FALSE; }
    BOOL rtl = GesherUiRtl(state->language);
    DWORD reading = rtl ? WS_EX_RTLREADING : 0;
    DWORD alignment = rtl ? SS_RIGHT : SS_LEFT;
    HWND heading = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "exact_alarm_title"),
        alignment, -1, 16, 12, 258, 24);
    HWND label = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "permission_app"),
        alignment, -1, 16, 43, 258, 14);
    HWND packages = PermissionControl(dialog, WS_EX_LTRREADING, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, EXACT_ALARM_PACKAGES, 16, 60, 258, 100);
    HWND allowed = PermissionControl(dialog, reading, L"BUTTON", GesherUiText(state->language, "exact_alarm_allow"),
        BS_AUTOCHECKBOX | WS_TABSTOP | (rtl ? BS_RIGHTBUTTON : 0), EXACT_ALARM_ALLOWED, 16, 92, 258, 24);
    HWND details = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language,
        state->count == 0 ? "exact_alarm_empty" : "exact_alarm_detail"), alignment, -1, 16, 124, 258, 52);
    HWND close = PermissionControl(dialog, 0, L"BUTTON", GesherUiText(state->language, "exact_alarm_close"),
        BS_DEFPUSHBUTTON | WS_TABSTOP, IDCANCEL, 108, 186, 76, 22);
    HWND apply = PermissionControl(dialog, 0, L"BUTTON", GesherUiText(state->language, "exact_alarm_apply"),
        BS_PUSHBUTTON | WS_TABSTOP, IDOK, 192, 186, 82, 22);
    if (!heading || !label || !packages || !allowed || !details || !close || !apply) {
      EndDialog(dialog, IDCANCEL); return FALSE;
    }
    GesherHostSetFont(heading, &state->heading_font, 140, FW_SEMIBOLD);
    for (int i = 0; i < state->count; ++i) {
      if (SendMessageW(packages, CB_ADDSTRING, 0, (LPARAM)state->packages[i]) < 0) {
        EndDialog(dialog, IDCANCEL); return FALSE;
      }
    }
    if (state->count > 0) {
      SendMessageW(packages, CB_SETCURSEL, 0, 0);
      SendMessageW(allowed, BM_SETCHECK, state->allowed[0] ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      EnableWindow(packages, FALSE); EnableWindow(allowed, FALSE); EnableWindow(apply, FALSE);
    }
    SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
    SetForegroundWindow(dialog);
    SetFocus(close);
    return FALSE;
  }
  if (state == NULL) return FALSE;
  if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
  if (message == WM_COMMAND) {
    int command = LOWORD(wparam);
    if (command == EXACT_ALARM_PACKAGES && HIWORD(wparam) == CBN_SELCHANGE) {
      LRESULT index = SendDlgItemMessageW(dialog, EXACT_ALARM_PACKAGES, CB_GETCURSEL, 0, 0);
      if (index >= 0 && index < state->count) SendDlgItemMessageW(dialog, EXACT_ALARM_ALLOWED,
          BM_SETCHECK, state->allowed[index] ? BST_CHECKED : BST_UNCHECKED, 0);
      return TRUE;
    }
    if (command == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (command == IDOK) {
      LRESULT index = SendDlgItemMessageW(dialog, EXACT_ALARM_PACKAGES, CB_GETCURSEL, 0, 0);
      if (index < 0 || index >= state->count) return TRUE;
      BOOL allowed = SendDlgItemMessageW(dialog, EXACT_ALARM_ALLOWED, BM_GETCHECK, 0, 0) == BST_CHECKED;
      state->result = (int)index * 2 + (allowed ? 1 : 0);
      EndDialog(dialog, IDOK);
      return TRUE;
    }
  }
  if (message == WM_DESTROY) {
    if (state->heading_font) DeleteObject(state->heading_font);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  return FALSE;
}

static int ExactAlarmDialog(enum GesherUiLanguage language, const WCHAR* const* packages,
        const unsigned char* allowed, int count) {
  ExactAlarmDialogState state = {0};
  state.language = language; state.packages = packages; state.allowed = allowed;
  state.count = count; state.result = -1;
  union { DWORD alignment; WORD words[128]; } buffer = {0};
  DLGTEMPLATE* dialog = (DLGTEMPLATE*)buffer.words;
  dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  dialog->dwExtendedStyle = GesherUiRtl(language) ? WS_EX_RTLREADING : 0;
  dialog->cx = 290; dialog->cy = 220;
  WORD* at = (WORD*)(dialog + 1);
  *at++ = 0; *at++ = 0;
  const WCHAR* title = GesherUiText(language, "exact_alarm_title");
  SIZE_T title_chars = wcslen(title) + 1;
  const WCHAR font[] = L"Segoe UI";
  if ((SIZE_T)(at - buffer.words) + title_chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) return -1;
  for (SIZE_T i = 0; i < title_chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, ExactAlarmDialogProc, (LPARAM)&state);
  return state.result;
}
