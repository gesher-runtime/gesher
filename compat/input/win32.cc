// Gesher: the input connection's Win32 calls, apart from the AOSP headers
// (connection.h, docs/input.md).

#include <windows.h>

#include <wchar.h>

#include <string>
#include <vector>

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

namespace {

std::string Narrow(const wchar_t* s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::vector<char> out(static_cast<size_t>(n));
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return std::string(out.data());
}

}  // namespace

std::vector<KeyboardLayout> KeyboardLayouts() {
  std::vector<KeyboardLayout> out;
  int n = GetKeyboardLayoutList(0, nullptr);
  if (n <= 0) return out;
  std::vector<HKL> list(static_cast<size_t>(n));
  n = GetKeyboardLayoutList(n, list.data());
  for (int i = 0; i < n; i++) {
    KeyboardLayout layout;
    layout.hkl = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(list[i]));
    const LCID lcid = MAKELCID(static_cast<WORD>(layout.hkl & 0xffff), SORT_DEFAULT);
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
      layout.locale = Narrow(name);
      wchar_t display[256];
      if (GetLocaleInfoEx(name, LOCALE_SLOCALIZEDDISPLAYNAME, display, 256) > 0) {
        layout.name = Narrow(display);
      }
    }
    out.push_back(layout);
  }
  return out;
}

uint64_t ThreadKeyboardLayout(void* hwnd) {
  DWORD thread = 0;
  if (hwnd != nullptr) thread = GetWindowThreadProcessId(static_cast<HWND>(hwnd), nullptr);
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(GetKeyboardLayout(thread)));
}

}  // namespace input
}  // namespace gesher
