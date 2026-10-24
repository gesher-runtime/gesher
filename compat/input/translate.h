// Gesher: Win32 window input to Android input events (docs/input.md).
// The translation is a pure function of the WindowEvents and the translator's
// state (buttons held, hovering, touch pointers, key repeats), so it is
// tested without a channel; connection.cc publishes what it produces.
#pragma once

#include <stdint.h>

#include <vector>

#include <input/Input.h>

#include "gesher_window.h"

namespace gesher {
namespace input {

// Device ids: keys come from Android's virtual keyboard (KeyCharacterMap
// VIRTUAL_KEYBOARD, whose map is Generic.kcm here), so
// KeyEvent.getUnicodeChar() and KeyCharacterMap.load(deviceId) find a map.
constexpr int32_t kKeyboardDeviceId = -1;
constexpr int32_t kMouseDeviceId = 1;
constexpr int32_t kTouchDeviceId = 2;
constexpr int32_t kStylusDeviceId = 3;

// What to publish.
struct AndroidEvent {
  // kText: a text event (text_input.h) for the connection's on_text_event,
  // in order with the input events.
  enum Kind { kKey, kMotion, kFocus, kText } kind = kKey;
  int32_t device_id = 0;
  int32_t source = 0;
  int32_t action = 0;
  int32_t flags = 0;
  int32_t meta_state = 0;
  int64_t down_time = 0;   // ns, SYSTEM_TIME_MONOTONIC
  int64_t event_time = 0;  // ns
  // Keys.
  int32_t key_code = 0;
  int32_t scan_code = 0;
  int32_t repeat_count = 0;
  // Motions.
  int32_t action_button = 0;
  int32_t button_state = 0;
  float cursor_x = 0, cursor_y = 0;  // AMOTION_EVENT_INVALID_CURSOR_POSITION for touch
  uint32_t pointer_count = 0;
  android::PointerProperties properties[MAX_POINTERS];
  android::PointerCoords coords[MAX_POINTERS];
  // Focus.
  bool has_focus = false;
  // Text: the connection's id of the text event.
  uint32_t text_id = 0;

  // A motion that can replace an earlier one still waiting to be sent (a
  // move or hover move of the same device and pointers).
  bool IsCoalescibleMove() const;
};

struct TranslateOptions {
  float scale = 1.0f;
  float offset_x = 0, offset_y = 0;
};

// Windows virtual-key code (with the scan code's extended bit, 0x100, which
// tells right Ctrl/Alt and the navigation block from the numeric keypad's
// keys) to Android key code; AKEYCODE_UNKNOWN if there is none.
int32_t KeyCodeFromVirtualKey(int32_t vk, int32_t scan);
// Windows scan code (set 1, 0x100 for E0-prefixed keys) to the Linux key code
// Android reports as KeyEvent.getScanCode(); 0 if unknown.
int32_t LinuxScanCode(int32_t scan);
// WindowEvent::modifiers (1 shift, 2 ctrl, 4 alt, 8 caps lock) to Android
// meta state (META_*_ON with the left-hand bits).
int32_t MetaState(int32_t modifiers);

class Translator {
 public:
  explicit Translator(const TranslateOptions& options) : options_(options) {}

  // Appends the Android events for `event` to `out` (none for events that
  // are not input: resize, close, characters). `now_ns` is the event's time
  // on SYSTEM_TIME_MONOTONIC.
  void Translate(const WindowEvent& event, int64_t now_ns, std::vector<AndroidEvent>* out);

  // Whether `event` is input this translator handles (false for kResize,
  // kClose and kChar).
  static bool IsInput(const WindowEvent& event);

 private:
  struct Touch {
    int32_t windows_id;
    int32_t android_id;
    float x, y;
  };

  void Mouse(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out);
  void Key(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out);
  void TouchEvent(const WindowEvent& event, int64_t t, std::vector<AndroidEvent>* out);
  AndroidEvent MouseMotion(int32_t action, float x, float y, int64_t t, int32_t meta) const;
  AndroidEvent TouchMotion(int32_t action, int64_t t, int32_t meta) const;
  float X(float x) const { return x * options_.scale + options_.offset_x; }
  float Y(float y) const { return y * options_.scale + options_.offset_y; }

  TranslateOptions options_;
  // Mouse.
  int32_t buttons_ = 0;  // AMOTION_EVENT_BUTTON_* held
  bool hovering_ = false;
  bool has_last_ = false;
  float last_x_ = 0, last_y_ = 0;  // window pixels
  int64_t mouse_down_time_ = 0;
  // Keys.
  int32_t last_key_ = -1;  // the key a repeat repeats
  int32_t repeat_ = 0;
  int64_t key_down_time_ = 0;
  // Touch.
  std::vector<Touch> touches_;  // by Android id
  int32_t touch_tool_ = 1;      // the gesture's first contact: 1 finger, 2 pen
  int64_t touch_down_time_ = 0;
};

}  // namespace input
}  // namespace gesher
