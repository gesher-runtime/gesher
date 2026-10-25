// Gesher: the input connection's Win32 calls (win32.cc), kept apart from the
// AOSP headers, which do not mix with windows.h (docs/input.md).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "gesher_window.h"

namespace gesher {
namespace input {

// The gesher::Window of a Gesher window's HWND in this process; null for any
// other window.
Window* WindowFromHwnd(void* hwnd);
// How long ago (ms, >= 0) a message whose GetMessageTime() is `message_time`
// was posted.
int64_t MessageAgeMs(int64_t message_time);
// PostMessageW; for tests.
bool PostWindowMessage(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam);
// ClientToScreen; for tests (wheel and pointer messages carry screen
// coordinates).
bool ClientToScreenPoint(void* hwnd, int32_t* x, int32_t* y);

// A keyboard layout (input language) of the session (GetKeyboardLayoutList).
struct KeyboardLayout {
  uint64_t hkl = 0;
  std::string locale;  // BCP 47 tag of its language (LCIDToLocaleName), "" if unknown
  std::string name;    // its language's display name (LOCALE_SLOCALIZEDDISPLAYNAME)
};
std::vector<KeyboardLayout> KeyboardLayouts();
// The keyboard layout of the thread of `hwnd` (0: this thread).
uint64_t ThreadKeyboardLayout(void* hwnd);

}  // namespace input
}  // namespace gesher
