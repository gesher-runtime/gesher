// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Win32 frame geometry and drawing only. No Window, Android events, COM or
// HWND ownership. All calls execute on the caller's existing window thread.
#pragma once
#include <windows.h>
#include <windowsx.h>
namespace gesher::window_presentation {
// The Back bar: an Android navigation bar's Back button under a top-level
// window's client area. It is non-client area, so the client area stays the
// app's display; the X closes the app's task, Back goes one screen back.
constexpr int kBackBarDip = 40;
constexpr LRESULT kHitBack = HTOBJECT;  // the Back button's hit-test code

inline int BackBarHeight(HWND hwnd) {
  return MulDiv(kBackBarDip, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

// The bar, in window coordinates.
inline RECT BackBarRect(HWND hwnd) {
  RECT client = {}, window = {};
  POINT origin = {0, 0};
  GetClientRect(hwnd, &client);
  ClientToScreen(hwnd, &origin);
  GetWindowRect(hwnd, &window);
  const int left = origin.x - window.left, top = origin.y - window.top + client.bottom;
  return {left, top, left + client.right, top + BackBarHeight(hwnd)};
}

// The Back button: the middle of the bar, as wide as two bar heights.
inline RECT BackButtonRect(HWND hwnd) {
  RECT bar = BackBarRect(hwnd);
  const int h = bar.bottom - bar.top, middle = (bar.left + bar.right) / 2;
  return {middle - h, bar.top, middle + h, bar.bottom};
}

// Draw into a caller-owned DC; restore selected GDI objects before returning.
inline void DrawBackBar(HDC dc, const RECT& bar, int state) {
  HBRUSH fill = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
  FillRect(dc, &bar, fill);
  DeleteObject(fill);
  const int h = bar.bottom - bar.top;
  const int cx = (bar.left + bar.right) / 2, cy = (bar.top + bar.bottom) / 2;
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
  if (state) {
    // Hot or pressed: a circle behind the button, as Android's ripple.
    HBRUSH ripple = CreateSolidBrush(state == 2 ? RGB(0x55, 0x55, 0x55) : RGB(0x3a, 0x3a, 0x3a));
    SelectObject(dc, ripple);
    const int r = h * 2 / 5;
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    DeleteObject(ripple);
  }
  // Android's Back: an outlined triangle pointing left.
  const int a = h / 4;
  POINT triangle[3] = {{cx - a * 4 / 5, cy}, {cx + a * 3 / 5, cy - a}, {cx + a * 3 / 5, cy + a}};
  HPEN pen = CreatePen(PS_SOLID, h / 20 > 1 ? h / 20 : 1, RGB(0xe8, 0xe8, 0xe8));
  SelectObject(dc, pen);
  Polygon(dc, triangle, 3);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
}

inline void PaintBackBar(HWND hwnd, int state) {
  if (IsIconic(hwnd)) return;
  const RECT bar = BackBarRect(hwnd);
  if (bar.right <= bar.left || bar.bottom <= bar.top) return;
  HDC dc = GetWindowDC(hwnd);
  if (dc == nullptr) return;
  DrawBackBar(dc, bar, state);
  ReleaseDC(hwnd, dc);
}

// Initial frame uses the original system-DPI API. Live resizing below uses
// the window DPI; do not add the Back bar until SetClientSize after creation.
struct InitialFrame {
  DWORD style;
  RECT bounds;
  int x, y;
};
inline InitialFrame BuildInitialFrame(bool popup, int width, int height) {
  InitialFrame frame{popup ? WS_POPUP : WS_OVERLAPPEDWINDOW, {0, 0, width, height},
                     popup ? 0 : CW_USEDEFAULT, popup ? 0 : CW_USEDEFAULT};
  AdjustWindowRectEx(&frame.bounds, frame.style, FALSE, 0);
  return frame;
}

inline RECT ClientFrameRect(HWND hwnd, bool popup, int width, int height) {
  RECT r = {0, 0, width, height};
  AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                         static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                         GetDpiForWindow(hwnd));
  if (!popup) r.bottom += BackBarHeight(hwnd);
  return r;
}

// Only called for geometry/hit-test messages on non-popup windows.
// Default Windows chrome, including close/minimize, is always drawn/tested first.
inline LRESULT FrameMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  LRESULT r = DefWindowProcW(hwnd, msg, wparam, lparam);
  if (msg == WM_NCCALCSIZE) {
    RECT* client = wparam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                          : reinterpret_cast<RECT*>(lparam);
    client->bottom -= BackBarHeight(hwnd);
    if (client->bottom < client->top) client->bottom = client->top;
  } else if (msg == WM_NCHITTEST) {
    if (r == HTCLIENT || IsIconic(hwnd)) return r;
    RECT window, button = BackButtonRect(hwnd), bar = BackBarRect(hwnd);
    GetWindowRect(hwnd, &window);
    POINT p = {GET_X_LPARAM(lparam) - window.left, GET_Y_LPARAM(lparam) - window.top};
    if (PtInRect(&button, p)) return kHitBack;
    return PtInRect(&bar, p) ? HTNOWHERE : r;
  }
  return r;
}
}  // namespace gesher::window_presentation
