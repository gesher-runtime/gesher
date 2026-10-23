// Gesher: the input connection's Win32 calls, apart from the AOSP headers
// (connection.h, docs/input.md).

#include <windows.h>

#include <wchar.h>

#include "win32.h"

namespace gesher {
namespace input {

Window* WindowFromHwnd(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (hwnd == nullptr || !IsWindow(hwnd)) return nullptr;
  wchar_t name[32];
  // gesher_window.cc's class; its window procedure keeps the Window in
  // GWLP_USERDATA from WM_NCCREATE on.
  if (GetClassNameW(hwnd, name, 32) == 0 || wcscmp(name, L"GesherWindow") != 0) return nullptr;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return nullptr;
  return reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int64_t MessageAgeMs(int64_t message_time) {
  // GetMessageTime() is a LONG of the tick count, which wraps.
  const DWORD age = GetTickCount() - static_cast<DWORD>(message_time);
  return age > 0x7fffffffu ? 0 : static_cast<int64_t>(age);
}

bool PostWindowMessage(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam) {
  return PostMessageW(static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wparam),
                      static_cast<LPARAM>(lparam)) != 0;
}

bool ClientToScreenPoint(void* hwnd, int32_t* x, int32_t* y) {
  POINT p = {*x, *y};
  if (!ClientToScreen(static_cast<HWND>(hwnd), &p)) return false;
  *x = p.x;
  *y = p.y;
  return true;
}

}  // namespace input
}  // namespace gesher
