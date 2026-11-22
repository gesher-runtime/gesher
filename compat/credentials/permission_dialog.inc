/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Included by account_crypto.c. Uses ordinary accessible Windows controls. */
#include "../windows/host_dialog_theme.h"

enum { PERMISSION_PACKAGE = 1101, PERMISSION_DETAILS = 1102, PERMISSION_BRAND = 1103,
       PERMISSION_HEADING = 1104 };

typedef struct {
  enum GesherUiLanguage language;
  const WCHAR* package_name;
  const WCHAR* permissions;
  HFONT heading_font;
  HFONT brand_font;
  HICON big_icon;
  HICON small_icon;
  GesherHostTheme theme;
  BOOL allowed;
} PermissionDialogState;

// Gesher's icon for the title bar and taskbar: gesher.ico next to the module
// holding this code (the installer puts it beside libgesher_account_crypto.so).
// Without the file the dialog keeps working, just without its own icon.
static void PermissionIcons(PermissionDialogState* state) {
  HMODULE module = NULL;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          (LPCWSTR)(UINT_PTR)&PermissionIcons, &module)) return;
  WCHAR path[MAX_PATH + 16];
  DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return;
  WCHAR* name = path;
  for (WCHAR* at = path; *at; ++at) if (*at == L'\\' || *at == L'/') name = at + 1;
  const WCHAR file[] = L"gesher.ico";
  for (SIZE_T i = 0; i < sizeof(file) / sizeof(file[0]); ++i) name[i] = file[i];
  state->big_icon = (HICON)LoadImageW(NULL, path, IMAGE_ICON,
      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
  state->small_icon = (HICON)LoadImageW(NULL, path, IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
}

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
        BS_OWNERDRAW | WS_TABSTOP, IDCANCEL, 76, 166, 76, 22);
    HWND allow = PermissionControl(dialog, 0, L"BUTTON", GesherUiText(state->language, "permission_allow"),
        BS_OWNERDRAW | WS_TABSTOP, IDYES, 160, 166, 74, 22);
    if (!brand || !heading || !app || !package || !requested || !details || !deny || !allow) {
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    GesherHostSetFont(heading, &state->heading_font, 145, FW_SEMIBOLD);
    GesherHostSetFont(brand, &state->brand_font, 110, FW_BOLD);
    SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
    if (state->big_icon) SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)state->big_icon);
    if (state->small_icon) SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)state->small_icon);
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
    if (command == IDYES || command == IDCANCEL) {
      state->allowed = command == IDYES;
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

static BOOL PermissionDialog(enum GesherUiLanguage language, const WCHAR* package_name, const WCHAR* permissions) {
  PermissionDialogState state = {0};
  state.language = language;
  state.package_name = package_name;
  state.permissions = permissions;
  union { DWORD alignment; WORD words[128]; } buffer = {0};
  DLGTEMPLATE* dialog = (DLGTEMPLATE*)buffer.words;
  // DS_MODALFRAME hides only the default icon, not one set with WM_SETICON.
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
      > sizeof(buffer.words) / sizeof(buffer.words[0])) return FALSE;
  for (SIZE_T i = 0; i < title_chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  // Failure, Esc, closing and the default button all deny. Only Allow changes state.
  PermissionIcons(&state);
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, PermissionDialogProc, (LPARAM)&state);
  // WM_SETICON does not take ownership; the window is gone now.
  if (state.big_icon) DestroyIcon(state.big_icon);
  if (state.small_icon) DestroyIcon(state.small_icon);
  return state.allowed;
}
