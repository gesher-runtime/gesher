/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/*
 * Native Gesher-host dialogs. Match the Control Center's light, blue-accent
 * surfaces without changing Windows control classes, accessibility, tab order,
 * or the default-deny behavior of permission/account prompts.
 *
 * Only Gesher-owned chrome is styled. Android application windows, OS common
 * dialogs, and system high-contrast colors are left to their respective owners.
 * This header is shared by C (system-server JNI) and C++ (the shell launcher).
 */
#ifndef GESHER_HOST_DIALOG_THEME_H
#define GESHER_HOST_DIALOG_THEME_H

#include <windows.h>
#include <wchar.h>

/* The project defines NOGDI for AOSP code. Includers must undefine it before
 * <windows.h> (as gesher_launch.cc and the -UNOGDI credential targets do). */
#ifdef NOGDI
#error "host_dialog_theme.h needs GDI: undefine NOGDI before including <windows.h>"
#endif

enum { GESHER_HOST_MESSAGE_TEXT = 1901, GESHER_HOST_MESSAGE_HEADING = 1902 };
/* Posted after a DPI change so a dialog can rebuild the fonts it owns once the
 * dialog manager has rescaled the dialog font. */
#define GESHER_HOST_REFONT (WM_APP + 0x1e1)

typedef struct {
  HBRUSH surface;
  HBRUSH input;
  BOOL high_contrast;
} GesherHostTheme;

static inline BOOL GesherHostHighContrast(void) {
  HIGHCONTRASTW contrast;
  ZeroMemory(&contrast, sizeof(contrast));
  contrast.cbSize = sizeof(contrast);
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
      && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

static inline COLORREF GesherHostTextColor(const GesherHostTheme* theme) {
  return theme->high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(16, 24, 40);
}

static inline COLORREF GesherHostAccentColor(const GesherHostTheme* theme) {
  return theme->high_contrast ? GetSysColor(COLOR_HOTLIGHT) : RGB(21, 112, 239);
}

/* DWM attributes are optional: Windows 10 and non-composited desktops retain
 * the normal title bar, and high-contrast overrides are never painted over. */
static inline void GesherHostApplyCaption(HWND window) {
  if (GesherHostHighContrast()) return;
  HMODULE module = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (module == NULL) return;
  typedef HRESULT (WINAPI *SetAttribute)(HWND, DWORD, LPCVOID, DWORD);
  SetAttribute set_attribute = (SetAttribute)GetProcAddress(module, "DwmSetWindowAttribute");
  if (set_attribute != NULL) {
    const DWORD corner = 2; /* DWMWCP_ROUND, Windows 11; ignored elsewhere. */
    const COLORREF caption = RGB(255, 255, 255);
    const COLORREF text = RGB(16, 24, 40);
    set_attribute(window, 33, &corner, sizeof(corner)); /* DWMWA_WINDOW_CORNER_PREFERENCE */
    set_attribute(window, 35, &caption, sizeof(caption)); /* DWMWA_CAPTION_COLOR */
    set_attribute(window, 36, &text, sizeof(text)); /* DWMWA_TEXT_COLOR */
  }
  FreeLibrary(module);
}

static inline void GesherHostThemeInit(GesherHostTheme* theme, HWND window) {
  theme->high_contrast = GesherHostHighContrast();
  theme->surface = theme->high_contrast ? NULL : CreateSolidBrush(RGB(255, 255, 255));
  theme->input = theme->high_contrast ? NULL : CreateSolidBrush(RGB(249, 250, 251));
  GesherHostApplyCaption(window);
}

static inline void GesherHostThemeDispose(GesherHostTheme* theme) {
  if (theme->surface != NULL) DeleteObject(theme->surface);
  if (theme->input != NULL) DeleteObject(theme->input);
  theme->surface = NULL;
  theme->input = NULL;
}

static inline HBRUSH GesherHostSurface(const GesherHostTheme* theme) {
  return theme->surface != NULL ? theme->surface : GetSysColorBrush(COLOR_WINDOW);
}

static inline HBRUSH GesherHostInput(const GesherHostTheme* theme) {
  return theme->input != NULL ? theme->input : GetSysColorBrush(COLOR_WINDOW);
}

/* A scaled/weighted copy of the dialog's current font; the caller owns it. */
static inline HFONT GesherHostFont(HWND dialog, int scale, LONG weight) {
  LOGFONTW font;
  ZeroMemory(&font, sizeof(font));
  HFONT original = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
  if (original == NULL || GetObjectW(original, sizeof(font), &font) == 0) return NULL;
  font.lfHeight = MulDiv(font.lfHeight, scale, 100);
  font.lfWeight = weight;
  return CreateFontIndirectW(&font);
}

/* Replace an owned control font (initially or after a DPI change). */
static inline void GesherHostSetFont(HWND control, HFONT* owned, int scale, LONG weight) {
  if (control == NULL) return;
  HFONT font = GesherHostFont(GetParent(control), scale, weight);
  if (font == NULL) return;
  SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
  if (*owned != NULL) DeleteObject(*owned);
  *owned = font;
}

/* The window's own DPI (per-monitor aware hosts), else the DC's system DPI. */
static inline int GesherHostDpi(HWND window, HDC dc) {
  typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  GetDpiForWindowFn get_dpi = user32 != NULL
      ? (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow") : NULL;
  UINT dpi = get_dpi != NULL ? get_dpi(window) : 0;
  return dpi != 0 ? (int)dpi : GetDeviceCaps(dc, LOGPIXELSX);
}

static inline BOOL GesherHostIsClass(HWND window, const WCHAR* klass) {
  WCHAR name[16];
  return window != NULL && GetClassNameW(window, name, 16) && lstrcmpiW(name, klass) == 0;
}

static inline void GesherHostDrawButton(const DRAWITEMSTRUCT* item,
                                        const GesherHostTheme* theme) {
  if (item->CtlType != ODT_BUTTON) return;
  /* IDOK is the single action of a notice or the chooser's selection. A grant
   * (IDYES) is drawn with the same weight as its safe default, Deny. */
  BOOL primary = item->CtlID == IDOK;
  BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
  BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
  /* The focused button is what Enter activates (see DM_GETDEFID below), so mark
   * it even before keyboard cues are shown. */
  BOOL focused = (item->itemState & ODS_FOCUS) != 0;
  int dpi = GesherHostDpi(item->hwndItem, item->hDC);
  COLORREF fill, border, ink;
  if (theme->high_contrast) {
    fill = GetSysColor(primary && !disabled ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
    border = GetSysColor(COLOR_WINDOWFRAME);
    ink = GetSysColor(disabled ? COLOR_GRAYTEXT
                             : primary ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
  } else {
    fill = disabled ? RGB(242, 244, 247)
        : primary ? (pressed ? RGB(23, 92, 211) : RGB(21, 112, 239))
                  : (pressed ? RGB(242, 244, 247) : RGB(255, 255, 255));
    border = primary && !disabled ? fill
        : focused && !disabled ? GesherHostAccentColor(theme) : RGB(208, 213, 221);
    ink = disabled ? RGB(152, 162, 179)
        : primary ? RGB(255, 255, 255) : RGB(52, 64, 84);
  }
  /* Paint the corners outside the rounded shape with the dialog surface. */
  FillRect(item->hDC, &item->rcItem, GesherHostSurface(theme));
  HBRUSH brush = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_INSIDEFRAME,
      MulDiv(focused && !primary && !disabled && !theme->high_contrast ? 2 : 1, dpi, 96), border);
  if (brush != NULL && pen != NULL) {
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RECT bounds = item->rcItem;
    int radius = MulDiv(12, dpi, 96);
    RoundRect(item->hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
  }
  if (brush != NULL) DeleteObject(brush);
  if (pen != NULL) DeleteObject(pen);
  RECT label = item->rcItem;
  InflateRect(&label, -MulDiv(5, dpi, 96), -MulDiv(2, dpi, 96));
  WCHAR text[256] = {0};
  GetWindowTextW(item->hwndItem, text, sizeof(text) / sizeof(text[0]));
  SetBkMode(item->hDC, TRANSPARENT);
  SetTextColor(item->hDC, ink);
  UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  if ((GetWindowLongPtrW(GetParent(item->hwndItem), GWL_EXSTYLE) & WS_EX_RTLREADING) != 0)
    flags |= DT_RTLREADING;
  DrawTextW(item->hDC, text, -1, &label, flags);
  if (focused && (item->itemState & ODS_NOFOCUSRECT) == 0) {
    RECT focus = item->rcItem;
    InflateRect(&focus, -MulDiv(4, dpi, 96), -MulDiv(4, dpi, 96));
    DrawFocusRect(item->hDC, &focus);
  }
}

/* Use only for the host's stock, owner-drawn buttons; Windows still provides
 * keyboard and UIA button behavior (Space, BM_CLICK and, via DM_GETDEFID, Enter). */
static inline INT_PTR GesherHostThemeEvent(HWND dialog, GesherHostTheme* theme,
                                           UINT message, WPARAM wparam, LPARAM lparam,
                                           int brand_id) {
  if (message == DM_GETDEFID) {
    /* The dialog manager only promotes BS_PUSHBUTTON controls to the default
     * button on focus. Keep that for owner-drawn buttons: Enter activates the
     * focused button; elsewhere it uses the dialog's own (safe) default. */
    HWND focus = GetFocus();
    if (focus != NULL && GetParent(focus) == dialog && GesherHostIsClass(focus, L"Button")
        && (GetWindowLongW(focus, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW
        && IsWindowEnabled(focus)) {
      SetWindowLongPtrW(dialog, DWLP_MSGRESULT,
                        MAKELONG(GetDlgCtrlID(focus), DC_HASDEFID));
      return TRUE;
    }
    return FALSE;
  }
  if (message == WM_DPICHANGED) {
    PostMessageW(dialog, GESHER_HOST_REFONT, 0, 0);
    return FALSE; /* Let the dialog manager rescale the dialog and its layout. */
  }
  if (message == WM_SYSCOLORCHANGE
      || (message == WM_SETTINGCHANGE && wparam == SPI_SETHIGHCONTRAST)) {
    /* Follow a high-contrast switch while the dialog is open. */
    GesherHostThemeDispose(theme);
    GesherHostThemeInit(theme, dialog);
    InvalidateRect(dialog, NULL, TRUE);
    return FALSE;
  }
  if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC
      || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX
      || message == WM_CTLCOLORBTN) {
    HDC dc = (HDC)wparam;
    /* Read-only EDITs ask with WM_CTLCOLORSTATIC; they are still input surfaces. */
    BOOL input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX
        || (message == WM_CTLCOLORSTATIC && GesherHostIsClass((HWND)lparam, L"Edit"));
    SetTextColor(dc, message == WM_CTLCOLORSTATIC
        && GetDlgCtrlID((HWND)lparam) == brand_id
        ? GesherHostAccentColor(theme) : GesherHostTextColor(theme));
    SetBkColor(dc, theme->high_contrast ? GetSysColor(COLOR_WINDOW)
               : input ? RGB(249, 250, 251) : RGB(255, 255, 255));
    return (INT_PTR)(input ? GesherHostInput(theme) : GesherHostSurface(theme));
  }
  if (message == WM_DRAWITEM && lparam != 0) {
    const DRAWITEMSTRUCT* item = (const DRAWITEMSTRUCT*)lparam;
    if (item->CtlType == ODT_BUTTON) {
      GesherHostDrawButton(item, theme);
      return TRUE;
    }
  }
  return FALSE;
}

/* Gesher's icon for a host dialog's title bar and taskbar button: gesher.ico
 * beside the module holding this code. The installer puts it in the same
 * folder as gesher-launch.exe and libgesher_account_crypto.so. Each module
 * compiles its own copy of this static function, so its address names that
 * module. Without the file the dialog keeps working, just without an icon. */
typedef struct {
  HICON big_icon;
  HICON small_icon;
} GesherHostIcons;

static inline void GesherHostLoadIcons(GesherHostIcons* icons) {
  icons->big_icon = NULL;
  icons->small_icon = NULL;
  HMODULE module = NULL;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)(UINT_PTR)&GesherHostLoadIcons, &module)) return;
  WCHAR path[MAX_PATH + 16];
  DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return;
  WCHAR* name = path;
  for (WCHAR* at = path; *at; ++at) if (*at == L'\\' || *at == L'/') name = at + 1;
  const WCHAR file[] = L"gesher.ico";
  for (SIZE_T i = 0; i < sizeof(file) / sizeof(file[0]); ++i) name[i] = file[i];
  icons->big_icon = (HICON)LoadImageW(NULL, path, IMAGE_ICON,
      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
  icons->small_icon = (HICON)LoadImageW(NULL, path, IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
}

/* DS_MODALFRAME hides only the default icon, not one set with WM_SETICON. */
static inline void GesherHostSetIcons(HWND dialog, const GesherHostIcons* icons) {
  if (icons->big_icon != NULL)
    SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)icons->big_icon);
  if (icons->small_icon != NULL)
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)icons->small_icon);
}

/* WM_SETICON does not take ownership: free once the dialog is gone. */
static inline void GesherHostFreeIcons(GesherHostIcons* icons) {
  if (icons->big_icon != NULL) DestroyIcon(icons->big_icon);
  if (icons->small_icon != NULL) DestroyIcon(icons->small_icon);
  icons->big_icon = NULL;
  icons->small_icon = NULL;
}

static inline HWND GesherHostControl(HWND dialog, DWORD extra, const WCHAR* klass,
                                      const WCHAR* text, DWORD style, int id,
                                      int x, int y, int width, int height) {
  RECT bounds = {x, y, x + width, y + height};
  if (!MapDialogRect(dialog, &bounds)) return NULL;
  HWND control = CreateWindowExW(extra, klass, text, WS_CHILD | WS_VISIBLE | style,
      bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
      dialog, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (control != NULL)
    SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
  return control;
}

typedef struct {
  const WCHAR* title;
  const WCHAR* message;
  const WCHAR* positive;
  const WCHAR* negative;
  BOOL rtl;
  BOOL accepted;
  HFONT heading_font;
  GesherHostTheme theme;
  GesherHostIcons icons;
} GesherHostPromptState;

/* Windows EDIT controls use CRLF line breaks; do not mutate input or embed it in
 * a resource template. The window copies this buffer synchronously at creation. */
static inline WCHAR* GesherHostEditCopy(const WCHAR* text) {
  /* Keep the dialog visible for very long (e.g. launch failure) text: show the
   * beginning and mark the cut instead of failing to present anything. */
  SIZE_T length = wcslen(text), extra = 0;
  BOOL cut = length > 32000;
  if (cut) {
    length = 32000;
    if (IS_HIGH_SURROGATE(text[length - 1])) --length;
  }
  for (SIZE_T i = 0; i < length; ++i)
    if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) ++extra;
  WCHAR* buffer = (WCHAR*)HeapAlloc(GetProcessHeap(), 0,
      (length + extra + 2) * sizeof(WCHAR));
  if (buffer == NULL) return NULL;
  SIZE_T at = 0;
  for (SIZE_T i = 0; i < length; ++i) {
    if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) buffer[at++] = L'\r';
    buffer[at++] = text[i];
  }
  if (cut) buffer[at++] = 0x2026; /* HORIZONTAL ELLIPSIS */
  buffer[at] = 0;
  return buffer;
}

static inline INT_PTR CALLBACK GesherHostPromptProc(HWND dialog, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
  GesherHostPromptState* state = (GesherHostPromptState*)GetWindowLongPtrW(dialog, DWLP_USER);
  if (message == WM_INITDIALOG) {
    state = (GesherHostPromptState*)lparam;
    SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
    GesherHostThemeInit(&state->theme, dialog);
    DWORD reading = state->rtl ? WS_EX_RTLREADING : 0;
    DWORD align = state->rtl ? SS_RIGHT : SS_LEFT;
    WCHAR* copy = GesherHostEditCopy(state->message);
    if (copy == NULL) { EndDialog(dialog, IDCANCEL); return FALSE; }
    HWND heading = GesherHostControl(dialog, reading, L"STATIC", state->title,
        align, GESHER_HOST_MESSAGE_HEADING, 16, 14, 300, 24);
    /* Text that fits is ordinary dialog text, which screen readers announce
     * with the dialog like a message box; longer text gets a scrolling,
     * selectable read-only box instead of overflowing. */
    RECT area = {16, 44, 316, 168};
    BOOL fits = FALSE;
    HDC dc = MapDialogRect(dialog, &area) ? GetDC(dialog) : NULL;
    if (dc != NULL) {
      HGDIOBJ old_font = SelectObject(dc, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0));
      RECT need = {0, 0, area.right - area.left, 0};
      DrawTextW(dc, copy, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL
                | DT_NOPREFIX | (state->rtl ? DT_RTLREADING : 0));
      fits = need.right <= area.right - area.left && need.bottom <= area.bottom - area.top;
      SelectObject(dc, old_font);
      ReleaseDC(dialog, dc);
    }
    HWND details = fits
        ? GesherHostControl(dialog, reading, L"STATIC", copy,
            align | SS_NOPREFIX | SS_EDITCONTROL, GESHER_HOST_MESSAGE_TEXT, 16, 44, 300, 124)
        : GesherHostControl(dialog, reading, L"EDIT", copy,
            ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER
            | WS_TABSTOP | (state->rtl ? ES_RIGHT : ES_LEFT),
            GESHER_HOST_MESSAGE_TEXT, 16, 44, 300, 124);
    SecureZeroMemory(copy, (wcslen(copy) + 1) * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, copy);
    /* Create Deny first so Tab order follows the on-screen order. */
    HWND deny = state->negative != NULL
        ? GesherHostControl(dialog, 0, L"BUTTON", state->negative,
            WS_TABSTOP | BS_OWNERDRAW, IDCANCEL, 134, 182, 86, 24) : NULL;
    HWND accept = GesherHostControl(dialog, 0, L"BUTTON", state->positive,
        WS_TABSTOP | BS_OWNERDRAW, state->negative ? IDYES : IDOK, 230, 182, 86, 24);
    if (!heading || !details || !accept || (state->negative && !deny)) {
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    GesherHostSetFont(heading, &state->heading_font, 145, FW_SEMIBOLD);
    SendMessageW(dialog, DM_SETDEFID, deny ? IDCANCEL : IDOK, 0);
    GesherHostSetIcons(dialog, &state->icons);
    // A host prompt may originate on a background system-server thread. Request
    // the foreground once, but never keep the dialog permanently topmost.
    if (!SetForegroundWindow(dialog)) {
      FLASHWINFO flash = {sizeof(flash), dialog, FLASHW_TRAY, 3, 0};
      FlashWindowEx(&flash);
    }
    SetFocus(deny ? deny : accept);
    return FALSE;
  }
  if (state == NULL) return FALSE;
  if (message == WM_DESTROY) {
    if (state->heading_font != NULL) DeleteObject(state->heading_font);
    GesherHostThemeDispose(&state->theme);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  if (message == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, GESHER_HOST_MESSAGE_HEADING),
                      &state->heading_font, 145, FW_SEMIBOLD);
    return TRUE;
  }
  if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
  if (message == WM_COMMAND) {
    int command = LOWORD(wparam);
    if (command == IDCANCEL || command == IDYES || command == IDOK) {
      state->accepted = command == IDYES || (command == IDOK && state->negative == NULL);
      EndDialog(dialog, command);
      return TRUE;
    }
  }
  return GesherHostThemeEvent(dialog, &state->theme, message, wparam, lparam,
                              GESHER_HOST_MESSAGE_HEADING);
}

/* A no/deny result (including Esc, X, and creation failures) is always false.
 * For informational windows, the caller ignores the return value. */
static inline BOOL GesherHostPrompt(const WCHAR* title, const WCHAR* message,
                                     const WCHAR* positive, const WCHAR* negative,
                                     BOOL rtl, HWND owner) {
  if (title == NULL || message == NULL || positive == NULL) return FALSE;
  GesherHostPromptState state;
  ZeroMemory(&state, sizeof(state));
  state.title = title;
  state.message = message;
  state.positive = positive;
  state.negative = negative;
  state.rtl = rtl;
  union { DWORD align; WORD words[256]; } buffer = {0};
  DLGTEMPLATE* layout = (DLGTEMPLATE*)buffer.words;
  layout->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  layout->dwExtendedStyle = rtl ? WS_EX_RTLREADING : 0;
  layout->cx = 332;
  layout->cy = 218;
  WORD* at = (WORD*)(layout + 1);
  *at++ = 0;
  *at++ = 0;
  SIZE_T chars = wcslen(title) + 1;
  const WCHAR font[] = L"Segoe UI";
  if ((SIZE_T)(at - buffer.words) + chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) return FALSE;
  for (SIZE_T i = 0; i < chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  GesherHostLoadIcons(&state.icons);
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), layout, owner, GesherHostPromptProc,
                          (LPARAM)&state);
  GesherHostFreeIcons(&state.icons);
  return state.accepted;
}
#endif /* GESHER_HOST_DIALOG_THEME_H */
