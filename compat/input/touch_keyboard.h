// Gesher: Windows' touch keyboard (touch_keyboard.cc). No windows.h here.
#pragma once

#include <stdint.h>

namespace gesher {
namespace input {

// Shows or hides the touch keyboard for window `hwnd` (InputPane.TryShow /
// TryHide): 1 when it did, 0 when Windows declined (a hardware keyboard, no
// touch keyboard on this edition), a failed HRESULT when InputPane is not
// there. On the window's thread.
int32_t ShowTouchKeyboard(void* hwnd, bool show);

}  // namespace input
}  // namespace gesher
