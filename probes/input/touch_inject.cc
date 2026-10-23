// The input probe's touch injection (input_probe.cc): a synthetic touch
// contact through InjectSyntheticPointerInput, which reaches the window as
// real WM_POINTER* messages. It needs an interactive desktop with the window
// on top at the point, so the probe reports the outcome as INFO only.
#include <windows.h>

namespace {

bool Inject(HSYNTHETICPOINTERDEVICE device, POINT p, POINTER_FLAGS flags) {
  POINTER_TYPE_INFO info = {};
  info.type = PT_TOUCH;
  POINTER_TOUCH_INFO& touch = info.touchInfo;
  touch.pointerInfo.pointerType = PT_TOUCH;
  touch.pointerInfo.pointerId = 0;
  touch.pointerInfo.ptPixelLocation = p;
  touch.pointerInfo.pointerFlags = flags;
  touch.touchFlags = TOUCH_FLAG_NONE;
  touch.touchMask = TOUCH_MASK_NONE;
  return InjectSyntheticPointerInput(device, &info, 1) != 0;
}

}  // namespace

// Shows the window on top and taps it at client (x, y), moving to (x + 20,
// y + 20) in between. Returns 0 when injected, else GetLastError().
unsigned long ProbeInjectTouch(void* hwnd_ptr, int x, int y) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  ShowWindow(hwnd, SW_SHOWNORMAL);
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(hwnd);
  Sleep(300);
  HSYNTHETICPOINTERDEVICE device = CreateSyntheticPointerDevice(PT_TOUCH, 1, POINTER_FEEDBACK_NONE);
  if (device == nullptr) return GetLastError();
  POINT p = {x, y};
  ClientToScreen(hwnd, &p);
  POINT q = {p.x + 20, p.y + 20};
  bool ok = Inject(device, p, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
  Sleep(30);
  ok = ok && Inject(device, q, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
  Sleep(30);
  ok = ok && Inject(device, q, POINTER_FLAG_UP);
  unsigned long error = ok ? 0 : GetLastError();
  if (error == 0 && !ok) error = 1;
  DestroySyntheticPointerDevice(device);
  return error;
}
