// Gesher: Win32 window input to Android input events. See translate.h and
// docs/input.md.

#include "translate.h"

#include <android/input.h>
#include <android/keycodes.h>

#include <algorithm>

namespace gesher {
namespace input {

using android::PointerCoords;
using android::PointerProperties;
using android::ToolType;

namespace {

// Windows virtual-key codes (winuser.h); the translation does not include
// windows.h, so it builds and tests anywhere.
enum : int32_t {
  kVkBack = 0x08, kVkTab = 0x09, kVkClear = 0x0C, kVkReturn = 0x0D, kVkShift = 0x10,
  kVkControl = 0x11, kVkMenu = 0x12, kVkPause = 0x13, kVkCapital = 0x14, kVkEscape = 0x1B,
  kVkSpace = 0x20, kVkPrior = 0x21, kVkNext = 0x22, kVkEnd = 0x23, kVkHome = 0x24,
  kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28, kVkSnapshot = 0x2C,
  kVkInsert = 0x2D, kVkDelete = 0x2E, kVkHelp = 0x2F, kVkLWin = 0x5B, kVkRWin = 0x5C,
  kVkApps = 0x5D, kVkSleep = 0x5F, kVkNumpad0 = 0x60, kVkMultiply = 0x6A, kVkAdd = 0x6B,
  kVkSeparator = 0x6C, kVkSubtract = 0x6D, kVkDecimal = 0x6E, kVkDivide = 0x6F, kVkF1 = 0x70,
  kVkF12 = 0x7B, kVkNumLock = 0x90, kVkScroll = 0x91, kVkLShift = 0xA0, kVkRShift = 0xA1,
  kVkLControl = 0xA2, kVkRControl = 0xA3, kVkLMenu = 0xA4, kVkRMenu = 0xA5,
  kVkBrowserBack = 0xA6, kVkBrowserForward = 0xA7, kVkBrowserRefresh = 0xA8,
  kVkBrowserSearch = 0xAA, kVkBrowserHome = 0xAC, kVkVolumeMute = 0xAD,
  kVkVolumeDown = 0xAE, kVkVolumeUp = 0xAF, kVkMediaNextTrack = 0xB0,
  kVkMediaPrevTrack = 0xB1, kVkMediaStop = 0xB2, kVkMediaPlayPause = 0xB3,
  kVkLaunchMail = 0xB4, kVkOem1 = 0xBA, kVkOemPlus = 0xBB, kVkOemComma = 0xBC,
  kVkOemMinus = 0xBD, kVkOemPeriod = 0xBE, kVkOem2 = 0xBF, kVkOem3 = 0xC0,
  kVkOem4 = 0xDB, kVkOem5 = 0xDC, kVkOem6 = 0xDD, kVkOem7 = 0xDE, kVkOem102 = 0xE2,
};

constexpr int32_t kExtended = 0x100;  // the scan code's E0 prefix

// WindowEvent buttons (1 left, 2 right, 4 middle) are Android's
// AMOTION_EVENT_BUTTON_PRIMARY, _SECONDARY and _TERTIARY already.
static_assert(AMOTION_EVENT_BUTTON_PRIMARY == 1 && AMOTION_EVENT_BUTTON_SECONDARY == 2 &&
              AMOTION_EVENT_BUTTON_TERTIARY == 4);

}  // namespace

int32_t KeyCodeFromVirtualKey(int32_t vk, int32_t scan) {
  const bool extended = (scan & kExtended) != 0;
  if (vk >= 'A' && vk <= 'Z') return AKEYCODE_A + (vk - 'A');
  if (vk >= '0' && vk <= '9') return AKEYCODE_0 + (vk - '0');
  if (vk >= kVkNumpad0 && vk <= kVkNumpad0 + 9) return AKEYCODE_NUMPAD_0 + (vk - kVkNumpad0);
  if (vk >= kVkF1 && vk <= kVkF12) return AKEYCODE_F1 + (vk - kVkF1);
  switch (vk) {
    case kVkBack: return AKEYCODE_DEL;
    case kVkTab: return AKEYCODE_TAB;
    case kVkClear: return AKEYCODE_CLEAR;
    case kVkReturn: return extended ? AKEYCODE_NUMPAD_ENTER : AKEYCODE_ENTER;
    case kVkShift: return (scan & 0xff) == 0x36 ? AKEYCODE_SHIFT_RIGHT : AKEYCODE_SHIFT_LEFT;
    case kVkLShift: return AKEYCODE_SHIFT_LEFT;
    case kVkRShift: return AKEYCODE_SHIFT_RIGHT;
    case kVkControl: return extended ? AKEYCODE_CTRL_RIGHT : AKEYCODE_CTRL_LEFT;
    case kVkLControl: return AKEYCODE_CTRL_LEFT;
    case kVkRControl: return AKEYCODE_CTRL_RIGHT;
    case kVkMenu: return extended ? AKEYCODE_ALT_RIGHT : AKEYCODE_ALT_LEFT;
    case kVkLMenu: return AKEYCODE_ALT_LEFT;
    case kVkRMenu: return AKEYCODE_ALT_RIGHT;
    case kVkPause: return AKEYCODE_BREAK;
    case kVkCapital: return AKEYCODE_CAPS_LOCK;
    case kVkEscape: return AKEYCODE_ESCAPE;
    case kVkSpace: return AKEYCODE_SPACE;
    // The navigation block; without the extended bit these are the numeric
    // keypad's keys with Num Lock off, which Android reports the same way.
    case kVkPrior: return AKEYCODE_PAGE_UP;
    case kVkNext: return AKEYCODE_PAGE_DOWN;
    case kVkEnd: return AKEYCODE_MOVE_END;
    case kVkHome: return AKEYCODE_MOVE_HOME;
    case kVkLeft: return AKEYCODE_DPAD_LEFT;
    case kVkUp: return AKEYCODE_DPAD_UP;
    case kVkRight: return AKEYCODE_DPAD_RIGHT;
    case kVkDown: return AKEYCODE_DPAD_DOWN;
    case kVkSnapshot: return AKEYCODE_SYSRQ;
    case kVkInsert: return AKEYCODE_INSERT;
    case kVkDelete: return AKEYCODE_FORWARD_DEL;
    case kVkHelp: return AKEYCODE_HELP;
    case kVkLWin: return AKEYCODE_META_LEFT;
    case kVkRWin: return AKEYCODE_META_RIGHT;
    case kVkApps: return AKEYCODE_MENU;
    case kVkSleep: return AKEYCODE_SLEEP;
    case kVkMultiply: return AKEYCODE_NUMPAD_MULTIPLY;
    case kVkAdd: return AKEYCODE_NUMPAD_ADD;
    case kVkSeparator: return AKEYCODE_NUMPAD_COMMA;
    case kVkSubtract: return AKEYCODE_NUMPAD_SUBTRACT;
    case kVkDecimal: return AKEYCODE_NUMPAD_DOT;
    case kVkDivide: return AKEYCODE_NUMPAD_DIVIDE;
    case kVkNumLock: return AKEYCODE_NUM_LOCK;
    case kVkScroll: return AKEYCODE_SCROLL_LOCK;
    case kVkBrowserBack: return AKEYCODE_BACK;
    case kVkBrowserForward: return AKEYCODE_FORWARD;
    case kVkBrowserRefresh: return AKEYCODE_REFRESH;
    case kVkBrowserSearch: return AKEYCODE_SEARCH;
    case kVkBrowserHome: return AKEYCODE_HOME;
    case kVkVolumeMute: return AKEYCODE_VOLUME_MUTE;
    case kVkVolumeDown: return AKEYCODE_VOLUME_DOWN;
    case kVkVolumeUp: return AKEYCODE_VOLUME_UP;
    case kVkMediaNextTrack: return AKEYCODE_MEDIA_NEXT;
    case kVkMediaPrevTrack: return AKEYCODE_MEDIA_PREVIOUS;
    case kVkMediaStop: return AKEYCODE_MEDIA_STOP;
    case kVkMediaPlayPause: return AKEYCODE_MEDIA_PLAY_PAUSE;
    case kVkLaunchMail: return AKEYCODE_ENVELOPE;
    // The US layout's punctuation keys, by position.
    case kVkOem1: return AKEYCODE_SEMICOLON;
    case kVkOemPlus: return AKEYCODE_EQUALS;
    case kVkOemComma: return AKEYCODE_COMMA;
    case kVkOemMinus: return AKEYCODE_MINUS;
    case kVkOemPeriod: return AKEYCODE_PERIOD;
    case kVkOem2: return AKEYCODE_SLASH;
    case kVkOem3: return AKEYCODE_GRAVE;
    case kVkOem4: return AKEYCODE_LEFT_BRACKET;
    case kVkOem5: return AKEYCODE_BACKSLASH;
    case kVkOem6: return AKEYCODE_RIGHT_BRACKET;
    case kVkOem7: return AKEYCODE_APOSTROPHE;
    case kVkOem102: return AKEYCODE_BACKSLASH;
    default: return AKEYCODE_UNKNOWN;
  }
}

int32_t LinuxScanCode(int32_t scan) {
  // Linux's key codes 1 to 88 are the PC's set 1 scan codes.
  if ((scan & kExtended) == 0) return scan > 0 && scan <= 0x58 ? scan : 0;
  switch (scan & 0xff) {
    case 0x1C: return 96;   // KEY_KPENTER
    case 0x1D: return 97;   // KEY_RIGHTCTRL
    case 0x35: return 98;   // KEY_KPSLASH
    case 0x37: return 99;   // KEY_SYSRQ
    case 0x38: return 100;  // KEY_RIGHTALT
    case 0x47: return 102;  // KEY_HOME
    case 0x48: return 103;  // KEY_UP
    case 0x49: return 104;  // KEY_PAGEUP
    case 0x4B: return 105;  // KEY_LEFT
    case 0x4D: return 106;  // KEY_RIGHT
    case 0x4F: return 107;  // KEY_END
    case 0x50: return 108;  // KEY_DOWN
    case 0x51: return 109;  // KEY_PAGEDOWN
    case 0x52: return 110;  // KEY_INSERT
    case 0x53: return 111;  // KEY_DELETE
    case 0x5B: return 125;  // KEY_LEFTMETA
    case 0x5C: return 126;  // KEY_RIGHTMETA
    case 0x5D: return 127;  // KEY_COMPOSE
    default: return 0;
  }
}

int32_t MetaState(int32_t modifiers) {
  int32_t meta = 0;
  if (modifiers & 1) meta |= AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON;
  if (modifiers & 2) meta |= AMETA_CTRL_ON | AMETA_CTRL_LEFT_ON;
  if (modifiers & 4) meta |= AMETA_ALT_ON | AMETA_ALT_LEFT_ON;
  if (modifiers & 8) meta |= AMETA_CAPS_LOCK_ON;
  return meta;
}

bool AndroidEvent::IsCoalescibleMove() const {
  return kind == kMotion &&
         (action == AMOTION_EVENT_ACTION_MOVE || action == AMOTION_EVENT_ACTION_HOVER_MOVE);
}

bool Translator::IsInput(const WindowEvent& event) {
  switch (event.type) {
    case WindowEvent::kMouseDown:
    case WindowEvent::kMouseUp:
    case WindowEvent::kMouseMove:
    case WindowEvent::kWheel:
    case WindowEvent::kMouseLeave:
    case WindowEvent::kKeyDown:
    case WindowEvent::kKeyUp:
    case WindowEvent::kFocus:
    case WindowEvent::kTouchDown:
    case WindowEvent::kTouchMove:
    case WindowEvent::kTouchUp:
    case WindowEvent::kTouchCancel:
    case WindowEvent::kOutside:
      return true;
    default:
      return false;
  }
}

void Translator::Translate(const WindowEvent& event, int64_t now_ns,
                           std::vector<AndroidEvent>* out) {
  switch (event.type) {
    case WindowEvent::kMouseDown:
    case WindowEvent::kMouseUp:
    case WindowEvent::kMouseMove:
    case WindowEvent::kWheel:
    case WindowEvent::kMouseLeave:
      Mouse(event, now_ns, out);
      break;
    case WindowEvent::kKeyDown:
    case WindowEvent::kKeyUp:
      Key(event, now_ns, out);
      break;
    case WindowEvent::kTouchDown:
    case WindowEvent::kTouchMove:
    case WindowEvent::kTouchUp:
    case WindowEvent::kTouchCancel:
      TouchEvent(event, now_ns, out);
      break;
    case WindowEvent::kOutside:
      Outside(event, now_ns, out);
      break;
    case WindowEvent::kFocus: {
      AndroidEvent e;
      e.kind = AndroidEvent::kFocus;
      e.has_focus = event.button != 0;
      e.event_time = now_ns;
      out->push_back(e);
      break;
    }
    default:
      break;
  }
}

AndroidEvent Translator::MouseMotion(int32_t action, float x, float y, int64_t t,
                                     int32_t meta) const {
  AndroidEvent e;
  e.kind = AndroidEvent::kMotion;
  e.device_id = kMouseDeviceId;
  e.source = AINPUT_SOURCE_MOUSE;
  e.action = action;
  e.meta_state = meta;
  e.button_state = buttons_;
  e.down_time = buttons_ != 0 || action == AMOTION_EVENT_ACTION_UP ? mouse_down_time_ : t;
  e.event_time = t;
  e.cursor_x = X(x);
  e.cursor_y = Y(y);
  e.pointer_count = 1;
  e.properties[0].clear();
  e.properties[0].id = 0;
  e.properties[0].toolType = ToolType::MOUSE;
  e.coords[0].clear();
  e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_X, X(x));
  e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, Y(y));
  e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, buttons_ != 0 ? 1.0f : 0.0f);
  return e;
}

void Translator::Mouse(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out) {
  const int32_t meta = MetaState(event.modifiers);
  const float x = event.x, y = event.y;
  switch (event.type) {
    case WindowEvent::kMouseMove: {
      // Windows repeats a move at the same place (after a click, a focus
      // change); Android needs one.
      if (has_last_ && x == last_x_ && y == last_y_) return;
      has_last_ = true;
      last_x_ = x;
      last_y_ = y;
      if (buttons_ != 0) {
        out->push_back(MouseMotion(AMOTION_EVENT_ACTION_MOVE, x, y, t, meta));
      } else if (!hovering_) {
        hovering_ = true;
        out->push_back(MouseMotion(AMOTION_EVENT_ACTION_HOVER_ENTER, x, y, t, meta));
      } else {
        out->push_back(MouseMotion(AMOTION_EVENT_ACTION_HOVER_MOVE, x, y, t, meta));
      }
      return;
    }
    case WindowEvent::kMouseDown: {
      const int32_t button = event.button & 7;
      if (button == 0 || (buttons_ & button) != 0) return;
      has_last_ = true;
      last_x_ = x;
      last_y_ = y;
      if (buttons_ == 0) {
        // As InputDispatcher does: the hover ends before the gesture starts.
        if (hovering_) {
          hovering_ = false;
          out->push_back(MouseMotion(AMOTION_EVENT_ACTION_HOVER_EXIT, x, y, t, meta));
        }
        mouse_down_time_ = t;
        buttons_ |= button;
        out->push_back(MouseMotion(AMOTION_EVENT_ACTION_DOWN, x, y, t, meta));
      } else {
        buttons_ |= button;
      }
      AndroidEvent press = MouseMotion(AMOTION_EVENT_ACTION_BUTTON_PRESS, x, y, t, meta);
      press.action_button = button;
      out->push_back(press);
      return;
    }
    case WindowEvent::kMouseUp: {
      const int32_t button = event.button & 7;
      if (button == 0 || (buttons_ & button) == 0) return;  // pressed outside the window
      has_last_ = true;
      last_x_ = x;
      last_y_ = y;
      buttons_ &= ~button;
      AndroidEvent release = MouseMotion(AMOTION_EVENT_ACTION_BUTTON_RELEASE, x, y, t, meta);
      release.action_button = button;
      if (buttons_ == 0) release.down_time = mouse_down_time_;
      out->push_back(release);
      if (buttons_ == 0) out->push_back(MouseMotion(AMOTION_EVENT_ACTION_UP, x, y, t, meta));
      return;
    }
    case WindowEvent::kWheel: {
      AndroidEvent e = MouseMotion(AMOTION_EVENT_ACTION_SCROLL, x, y, t, meta);
      e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_VSCROLL, event.delta);
      e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_HSCROLL, event.delta_x);
      out->push_back(e);
      return;
    }
    case WindowEvent::kMouseLeave:
      if (hovering_) {
        hovering_ = false;
        out->push_back(MouseMotion(AMOTION_EVENT_ACTION_HOVER_EXIT, last_x_, last_y_, t, meta));
      }
      has_last_ = false;
      return;
    default:
      return;
  }
}

void Translator::Outside(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out) {
  // ACTION_OUTSIDE: one event, at the press's place in this window (the same
  // app's windows: InputDispatcher zeroes the coordinates only across uids).
  // It starts and ends nothing, so the translator's own state stays.
  const bool touch = event.tool != 0;
  const bool pen = event.tool == 2;
  AndroidEvent e;
  e.kind = AndroidEvent::kMotion;
  e.device_id = touch ? (pen ? kStylusDeviceId : kTouchDeviceId) : kMouseDeviceId;
  e.source = !touch ? AINPUT_SOURCE_MOUSE
             : pen  ? (AINPUT_SOURCE_STYLUS | AINPUT_SOURCE_TOUCHSCREEN)
                    : AINPUT_SOURCE_TOUCHSCREEN;
  e.action = AMOTION_EVENT_ACTION_OUTSIDE;
  e.meta_state = MetaState(event.modifiers);
  e.down_time = t;
  e.event_time = t;
  e.cursor_x = touch ? android::AMOTION_EVENT_INVALID_CURSOR_POSITION : X(event.x);
  e.cursor_y = touch ? android::AMOTION_EVENT_INVALID_CURSOR_POSITION : Y(event.y);
  e.pointer_count = 1;
  e.properties[0].clear();
  e.properties[0].id = 0;
  e.properties[0].toolType = !touch ? ToolType::MOUSE : pen ? ToolType::STYLUS : ToolType::FINGER;
  e.coords[0].clear();
  e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_X, X(event.x));
  e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, Y(event.y));
  e.coords[0].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
  out->push_back(e);
}

void Translator::Key(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out) {
  AndroidEvent e;
  e.kind = AndroidEvent::kKey;
  e.device_id = kKeyboardDeviceId;
  e.source = AINPUT_SOURCE_KEYBOARD;
  e.flags = AKEY_EVENT_FLAG_FROM_SYSTEM;
  e.key_code = KeyCodeFromVirtualKey(event.key, event.scan);
  e.scan_code = LinuxScanCode(event.scan);
  e.meta_state = MetaState(event.modifiers);
  e.event_time = t;
  if (event.type == WindowEvent::kKeyDown) {
    e.action = AKEY_EVENT_ACTION_DOWN;
    if (event.repeat != 0 && event.key == last_key_) {
      repeat_++;
    } else {
      repeat_ = 0;
      key_down_time_ = t;
    }
    last_key_ = event.key;
    e.repeat_count = repeat_;
    e.down_time = key_down_time_;
  } else {
    e.action = AKEY_EVENT_ACTION_UP;
    e.down_time = event.key == last_key_ ? key_down_time_ : t;
    if (event.key == last_key_) {
      last_key_ = -1;
      repeat_ = 0;
    }
  }
  out->push_back(e);
}

AndroidEvent Translator::TouchMotion(int32_t action, int64_t t, int32_t meta) const {
  AndroidEvent e;
  e.kind = AndroidEvent::kMotion;
  const bool pen = touch_tool_ == 2;
  e.device_id = pen ? kStylusDeviceId : kTouchDeviceId;
  e.source = pen ? (AINPUT_SOURCE_STYLUS | AINPUT_SOURCE_TOUCHSCREEN) : AINPUT_SOURCE_TOUCHSCREEN;
  e.action = action;
  e.meta_state = meta;
  e.down_time = touch_down_time_;
  e.event_time = t;
  e.cursor_x = android::AMOTION_EVENT_INVALID_CURSOR_POSITION;
  e.cursor_y = android::AMOTION_EVENT_INVALID_CURSOR_POSITION;
  e.pointer_count = static_cast<uint32_t>(touches_.size());
  for (size_t i = 0; i < touches_.size(); i++) {
    e.properties[i].clear();
    e.properties[i].id = touches_[i].android_id;
    e.properties[i].toolType = pen ? ToolType::STYLUS : ToolType::FINGER;
    e.coords[i].clear();
    e.coords[i].setAxisValue(AMOTION_EVENT_AXIS_X, X(touches_[i].x));
    e.coords[i].setAxisValue(AMOTION_EVENT_AXIS_Y, Y(touches_[i].y));
    e.coords[i].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
  }
  return e;
}

void Translator::TouchEvent(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out) {
  const int32_t meta = MetaState(event.modifiers);
  auto it = std::find_if(touches_.begin(), touches_.end(),
                         [&](const Touch& touch) { return touch.windows_id == event.pointer; });
  switch (event.type) {
    case WindowEvent::kTouchDown: {
      if (it != touches_.end() || touches_.size() >= MAX_POINTERS) return;
      // The lowest Android id not in use, as InputReader assigns them.
      int32_t id = 0;
      while (std::any_of(touches_.begin(), touches_.end(),
                         [id](const Touch& touch) { return touch.android_id == id; })) {
        id++;
      }
      const bool first = touches_.empty();
      if (first) {
        touch_tool_ = event.tool == 2 ? 2 : 1;
        touch_down_time_ = t;
      }
      Touch touch{event.pointer, id, event.x, event.y};
      auto pos = std::find_if(touches_.begin(), touches_.end(),
                              [id](const Touch& other) { return other.android_id > id; });
      const int32_t index = static_cast<int32_t>(pos - touches_.begin());
      touches_.insert(pos, touch);
      out->push_back(TouchMotion(
          first ? AMOTION_EVENT_ACTION_DOWN
                : AMOTION_EVENT_ACTION_POINTER_DOWN |
                      (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
          t, meta));
      return;
    }
    case WindowEvent::kTouchMove:
      if (it == touches_.end() || (it->x == event.x && it->y == event.y)) return;
      it->x = event.x;
      it->y = event.y;
      out->push_back(TouchMotion(AMOTION_EVENT_ACTION_MOVE, t, meta));
      return;
    case WindowEvent::kTouchUp: {
      if (it == touches_.end()) return;
      it->x = event.x;
      it->y = event.y;
      const int32_t index = static_cast<int32_t>(it - touches_.begin());
      out->push_back(TouchMotion(
          touches_.size() == 1 ? AMOTION_EVENT_ACTION_UP
                               : AMOTION_EVENT_ACTION_POINTER_UP |
                                     (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
          t, meta));
      touches_.erase(touches_.begin() + index);
      return;
    }
    case WindowEvent::kTouchCancel: {
      if (touches_.empty()) return;
      AndroidEvent e = TouchMotion(AMOTION_EVENT_ACTION_CANCEL, t, meta);
      e.flags |= AMOTION_EVENT_FLAG_CANCELED;
      out->push_back(e);
      touches_.clear();
      return;
    }
    default:
      return;
  }
}

}  // namespace input
}  // namespace gesher
