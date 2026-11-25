/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Included after permission_dialog.inc (PermissionControl, host theme).
 * Pre-launch permission review for legacy applications (targetSdk < 23):
 * all requested runtime permission groups start enabled and the user may
 * toggle any group off before continuing, or cancel without launching. */

enum { REVIEW_PACKAGE = 1401, REVIEW_BRAND = 1402, REVIEW_HEADING = 1403,
       REVIEW_SCROLL = 1404, REVIEW_GROUP_BASE = 1500 };
// Keep these values in sync with PermissionRequests.java (UNAVAILABLE, DISMISSED, ALLOWED).
enum { REVIEW_UNAVAILABLE = -1, REVIEW_DISMISSED = 0, REVIEW_ALLOWED = 2 };
enum { REVIEW_MAX_GROUPS = 32, REVIEW_VISIBLE_ROWS = 8 };

typedef struct {
  enum GesherUiLanguage language;
  const WCHAR* package_name;
  const WCHAR* const* groups;
  unsigned char* enabled;
  int count;
  int row_height;
  int scroll_pos;
  int max_scroll;
  HWND scroll_bar;
  HFONT heading_font;
  HFONT brand_font;
  GesherHostTheme theme;
  GesherHostIcons icons;
  int decision;
} ReviewDialogState;

static void ReviewLayoutRows(HWND dialog, ReviewDialogState* state, int next_pos) {
  if (next_pos < 0) next_pos = 0;
  if (next_pos > state->max_scroll) next_pos = state->max_scroll;
  state->scroll_pos = next_pos;
  if (state->scroll_bar) SetScrollPos(state->scroll_bar, SB_CTL, state->scroll_pos, TRUE);
  int visible_rows = state->count > REVIEW_VISIBLE_ROWS ? REVIEW_VISIBLE_ROWS : state->count;
  int width = state->max_scroll > 0 ? 204 : 218;
  for (int i = 0; i < state->count; ++i) {
    HWND box = GetDlgItem(dialog, REVIEW_GROUP_BASE + i);
    if (!box) continue;
    int row = i - state->scroll_pos;
    if (row >= 0 && row < visible_rows) {
      RECT r = {16, 128 + row * state->row_height, 16 + width, 128 + (row + 1) * state->row_height};
      MapDialogRect(dialog, &r);
      SetWindowPos(box, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      ShowWindow(box, SW_HIDE);
    }
  }
}

static BOOL ReviewCreateControls(HWND dialog, ReviewDialogState* state) {
  GesherHostThemeInit(&state->theme, dialog);
  BOOL rtl = GesherUiRtl(state->language);
  DWORD reading = rtl ? WS_EX_RTLREADING : 0;
  DWORD alignment = rtl ? SS_RIGHT : SS_LEFT;
  HWND brand = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "brand"),
      alignment, REVIEW_BRAND, 16, 10, 218, 14);
  HWND heading = PermissionControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "review_permission_heading"),
      alignment, REVIEW_HEADING, 16, 28, 218, 24);
  HWND app = PermissionControl(dialog, reading, L"STATIC", GesherUiText(state->language, "permission_app"),
      alignment, -1, 16, 58, 218, 14);
  // The authenticated package ID has its own LTR control, without bidi isolates.
  HWND package = PermissionControl(dialog, WS_EX_LTRREADING, L"EDIT", state->package_name,
      ES_READONLY | ES_AUTOHSCROLL | ES_LEFT | WS_BORDER | WS_TABSTOP, REVIEW_PACKAGE, 16, 74, 218, 18);
  HWND requested = PermissionControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "review_permission_requested"), alignment, -1, 16, 100, 218, 24);
  if (!brand || !heading || !app || !package || !requested) return FALSE;

  int visible_rows = state->count > REVIEW_VISIBLE_ROWS ? REVIEW_VISIBLE_ROWS : state->count;
  int list_height = visible_rows * state->row_height;
  state->scroll_pos = 0;
  state->max_scroll = state->count > REVIEW_VISIBLE_ROWS ? state->count - REVIEW_VISIBLE_ROWS : 0;
  int box_width = state->max_scroll > 0 ? 204 : 218;
  DWORD checkbox_style = BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP | (rtl ? BS_RIGHTBUTTON : 0);
  for (int i = 0; i < state->count; ++i) {
    int y = 128 + (i < visible_rows ? i : 0) * state->row_height;
    HWND box = PermissionControl(dialog, reading, L"BUTTON", state->groups[i], checkbox_style,
        REVIEW_GROUP_BASE + i, 16, y, box_width, state->row_height);
    if (!box) return FALSE;
    SendMessageW(box, BM_SETCHECK, state->enabled[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    if (i >= visible_rows) ShowWindow(box, SW_HIDE);
  }
  if (state->max_scroll > 0) {
    state->scroll_bar = PermissionControl(dialog, 0, L"SCROLLBAR", L"",
        SBS_VERT | WS_TABSTOP, REVIEW_SCROLL, 224, 128, 10, list_height);
    if (!state->scroll_bar) return FALSE;
    SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, state->count - 1,
        (UINT)REVIEW_VISIBLE_ROWS, 0, 0};
    SetScrollInfo(state->scroll_bar, SB_CTL, &info, TRUE);
  }

  int button_y = 128 + list_height + 10;
  HWND cancel = PermissionControl(dialog, 0, L"BUTTON",
      GesherUiText(state->language, "review_permission_cancel"),
      BS_OWNERDRAW | WS_TABSTOP, IDCANCEL, 76, button_y, 76, 22);
  HWND confirm = PermissionControl(dialog, 0, L"BUTTON",
      GesherUiText(state->language, "review_permission_confirm"),
      BS_OWNERDRAW | WS_TABSTOP, IDOK, 160, button_y, 74, 22);
  if (!cancel || !confirm) return FALSE;

  GesherHostSetFont(heading, &state->heading_font, 145, FW_SEMIBOLD);
  GesherHostSetFont(brand, &state->brand_font, 110, FW_BOLD);
  return TRUE;
}

static INT_PTR CALLBACK ReviewDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  ReviewDialogState* state = (ReviewDialogState*)GetWindowLongPtrW(dialog, DWLP_USER);
  if (message == WM_INITDIALOG) {
    state = (ReviewDialogState*)lparam;
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
    if (previous == 0 && GetLastError() != 0) {
      state->decision = REVIEW_UNAVAILABLE;
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    if (!ReviewCreateControls(dialog, state)) {
      state->decision = REVIEW_UNAVAILABLE;
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    HWND confirm = GetDlgItem(dialog, IDOK);
    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
    GesherHostSetIcons(dialog, &state->icons);
    // Request foreground once; never keep the consent window permanently topmost.
    if (!SetForegroundWindow(dialog)) {
      FLASHWINFO flash = {sizeof(flash), dialog, FLASHW_TRAY, 3, 0};
      FlashWindowEx(&flash);
    }
    SetFocus(confirm);
    return FALSE;
  }
  if (state == NULL) return FALSE;
  if (message == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, REVIEW_HEADING), &state->heading_font, 145, FW_SEMIBOLD);
    GesherHostSetFont(GetDlgItem(dialog, REVIEW_BRAND), &state->brand_font, 110, FW_BOLD);
    return TRUE;
  }
  if (message == WM_CLOSE) {
    state->decision = REVIEW_DISMISSED;
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
  if (message == WM_VSCROLL && state->max_scroll > 0) {
    SCROLLINFO info = {sizeof(info), SIF_ALL, 0, 0, 0, 0, 0};
    GetScrollInfo(state->scroll_bar, SB_CTL, &info);
    int next = state->scroll_pos;
    switch (LOWORD(wparam)) {
      case SB_LINEUP: --next; break;
      case SB_LINEDOWN: ++next; break;
      case SB_PAGEUP: next -= (int)info.nPage; break;
      case SB_PAGEDOWN: next += (int)info.nPage; break;
      case SB_THUMBTRACK:
      case SB_THUMBPOSITION: next = info.nTrackPos; break;
      default: break;
    }
    ReviewLayoutRows(dialog, state, next);
    return TRUE;
  }
  if (message == WM_MOUSEWHEEL && state->max_scroll > 0) {
    int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    ReviewLayoutRows(dialog, state, state->scroll_pos + (delta > 0 ? -1 : 1));
    return TRUE;
  }
  if (message == WM_COMMAND) {
    int command = LOWORD(wparam);
    if (command == IDOK) {
      for (int i = 0; i < state->count; ++i) {
        state->enabled[i] = SendDlgItemMessageW(dialog, REVIEW_GROUP_BASE + i,
            BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
      }
      state->decision = REVIEW_ALLOWED;
      EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (command == IDCANCEL) {
      state->decision = REVIEW_DISMISSED;
      EndDialog(dialog, IDCANCEL);
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
  return GesherHostThemeEvent(dialog, &state->theme, message, wparam, lparam, REVIEW_BRAND);
}

static int ReviewPermissionDialog(enum GesherUiLanguage language, const WCHAR* package_name,
        const WCHAR* const* groups, unsigned char* enabled, int count) {
  if (package_name == NULL || groups == NULL || enabled == NULL
      || count <= 0 || count > REVIEW_MAX_GROUPS) {
    return REVIEW_UNAVAILABLE;
  }
  ReviewDialogState state = {0};
  state.language = language;
  state.package_name = package_name;
  state.groups = groups;
  state.enabled = enabled;
  state.count = count;
  state.row_height = 18;
  state.decision = REVIEW_DISMISSED;

  int visible_rows = count > REVIEW_VISIBLE_ROWS ? REVIEW_VISIBLE_ROWS : count;
  int button_y = 128 + visible_rows * state.row_height + 10;
  union { DWORD alignment; WORD words[128]; } buffer = {0};
  DLGTEMPLATE* dialog = (DLGTEMPLATE*)buffer.words;
  dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  dialog->dwExtendedStyle = GesherUiRtl(language) ? WS_EX_RTLREADING : 0;
  dialog->cx = 250;
  dialog->cy = (short)(button_y + 32);
  WORD* at = (WORD*)(dialog + 1);
  *at++ = 0; // No menu.
  *at++ = 0; // Default dialog class.
  const WCHAR* title = GesherUiText(language, "review_permission_title");
  SIZE_T title_chars = wcslen(title) + 1;
  const WCHAR font[] = L"Segoe UI";
  if ((SIZE_T)(at - buffer.words) + title_chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) return REVIEW_UNAVAILABLE;
  for (SIZE_T i = 0; i < title_chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  GesherHostLoadIcons(&state.icons);
  INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, ReviewDialogProc,
      (LPARAM)&state);
  GesherHostFreeIcons(&state.icons);
  return result == -1 ? REVIEW_UNAVAILABLE : state.decision;
}
