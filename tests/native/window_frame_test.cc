// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Real Win32 frame helpers on a never-displayed private desktop; no Android VM.
#include "../../compat/hwui/window/window_frame.h"
#include <cstdio>
#include <initializer_list>
namespace view = gesher::window_presentation;
static int checks, failures;
static void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}
static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) &&
      (msg == WM_NCCALCSIZE || msg == WM_NCHITTEST))
    return view::FrameMessage(hwnd, msg, w, l);
  return DefWindowProcW(hwnd, msg, w, l);
}
int main() {
  HDESK original = GetThreadDesktop(GetCurrentThreadId());
  wchar_t name[64]; swprintf(name, 64, L"GesherFrameTest-%lu", GetCurrentProcessId());
  HDESK desktop = CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  if (!desktop || !SetThreadDesktop(desktop)) return 2;
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherFrameFixture";
  if (!RegisterClassW(&wc)) return 2;
  for (bool popup : {false, true}) {
    auto frame = view::BuildInitialFrame(popup, 360, 640);
    Check(frame.style == (popup ? WS_POPUP : WS_OVERLAPPEDWINDOW), "initial style");
    Check(frame.x == (popup ? 0 : CW_USEDEFAULT) && frame.y == frame.x, "initial position");
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"fixture", frame.style, 30, 40,
        frame.bounds.right-frame.bounds.left, frame.bounds.bottom-frame.bounds.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, "create real window");
    if (!hwnd) continue;
    if (!popup) GesherSetNonclientRtl(hwnd, TRUE);
    for (const SIZE size : {SIZE{360,640}, SIZE{800,600}, SIZE{1,1}}) {
      RECT bounds = view::ClientFrameRect(hwnd, popup, size.cx, size.cy);
      SetWindowPos(hwnd, nullptr, 0, 0, bounds.right-bounds.left, bounds.bottom-bounds.top,
          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      RECT client{}; GetClientRect(hwnd, &client);
      // Windows enforces its own minimum frame size for tiny top-level windows.
      if (size.cx > 1) Check(client.right == size.cx && client.bottom == size.cy, "exact Android client size");
      if (!popup && size.cx > 1) {
        RECT window{}; GetWindowRect(hwnd, &window);
        RECT button = view::BackButtonRect(hwnd), bar = view::BackBarRect(hwnd);
        auto hit = [&](int x, int y) { return SendMessageW(hwnd, WM_NCHITTEST, 0,
            MAKELPARAM(window.left+x, window.top+y)); };
        Check(hit((button.left+button.right)/2, (button.top+button.bottom)/2) == view::kHitBack, "Back hit");
        Check(hit(bar.left+1, (bar.top+bar.bottom)/2) == HTNOWHERE, "bar is not Android client");
        POINT origin{}; ClientToScreen(hwnd, &origin);
        Check(SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(origin.x+10, origin.y+10)) == HTCLIENT, "client hit preserved");
        Check(button.right-button.left == 2*view::BackBarHeight(hwnd), "button bounds at window DPI");
      }
    }
    DestroyWindow(hwnd);
  }
  HDC dc = CreateCompatibleDC(nullptr);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth=240; info.bmiHeader.biHeight=-80;
  info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
  void* pixels=nullptr; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
  HGDIOBJ old=SelectObject(dc,bitmap);
  RECT warmup{0,0,240,40};
  view::DrawBackBar(dc,warmup,2); // Initialize GDI stock objects before the census.
  DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
  for (int height : {40,60,80}) for (int state : {0,1,2}) {
    RECT bar{0,0,240,height};
    for (int i=0;i<100;++i) view::DrawBackBar(dc,bar,state);
    Check(GetPixel(dc,1,1)==RGB(0x20,0x20,0x20),"bar background");
    COLORREF center=state==2 ? RGB(0x55,0x55,0x55) : state==1 ? RGB(0x3a,0x3a,0x3a) : RGB(0x20,0x20,0x20);
    Check(GetPixel(dc,120,height/2)==center,"idle/hot/pressed center");
    bool triangle=false;
    for(int y=0;y<height;++y) for(int x=100;x<140;++x)
      if(GetPixel(dc,x,y)==RGB(0xe8,0xe8,0xe8)) triangle=true;
    Check(triangle,"Back triangle drawn");
  }
  for (bool rtl : {false, true}) {
    view::DrawBackBar(dc, warmup, 0, rtl);
    const COLORREF ink = RGB(0xe8, 0xe8, 0xe8);
    Check(GetPixel(dc, rtl ? 128 : 112, 20) == ink, "Back points toward the leading edge");
    Check(GetPixel(dc, rtl ? 112 : 128, 20) != ink, "Back has no second arrow tip");
  }
  view::DrawBackBar(dc, warmup, 0, false, true);
  Check(GetPixel(dc, 1, 1) == GetSysColor(COLOR_WINDOW), "high contrast uses the system surface");
  Check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==before,"paint releases GDI objects");
  SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
  UnregisterClassW(wc.lpszClassName,wc.hInstance);
  SetThreadDesktop(original); CloseDesktop(desktop);
  std::printf("window_frame_test: %d checks, %d failures\n",checks,failures);
  return failures ? 1 : 0;
}
