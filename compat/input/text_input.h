// Gesher: text input for a window's focused editor (docs/input.md, "Text
// input"; ADR 0039). Shared by the window procedure's IME side (ime.cc, which
// includes windows.h) and the connection (connection.cc, AOSP's headers), so
// it includes neither.
#pragma once

#include <stdint.h>

#include <string>

namespace gesher {
namespace input {

// What the window's input method gives the focused editor, in the order of
// the window's key events: text from WM_CHAR/WM_UNICHAR (other keyboard
// layouts, dead keys, SendInput's VK_PACKET, the emoji panel, dictation) and
// IMM32 compositions, and the Enter key as the editor's action.
struct TextEvent {
  enum Kind : int32_t {
    kCommit = 1,        // `text` is typed (InputConnection.commitText)
    kCompose = 2,       // `text` is the composition so far (setComposingText)
    kComposeEnd = 3,    // the composition ended (finishComposingText)
    kEditorAction = 4,  // Enter: the editor's IME action (performEditorAction)
    kDocumentReplace = 5, kDocumentSelect = 6,
    kDocumentCompositionRange = 7, kDocumentCompositionEnd = 8,
    kDocumentCompositionText = 9,
  };
  int32_t kind = kCommit;
  std::u16string text;
  int32_t cursor = 0;         // kCompose: the caret in `text`, in UTF-16 units
  int32_t target_start = -1;  // kCompose: the clause being converted, or -1
  int32_t target_end = -1;
  int32_t epoch = 0, revision = 0, start = 0, end = 0;
};

// What the focused editor takes (WindowInput.setTextInput).
struct TextInputState {
  bool active = false;       // an editor with an InputConnection has the focus
  bool ime = false;          // compositions are allowed (not a password or number field)
  int32_t enter_action = 0;  // EditorInfo.IME_ACTION_* Enter performs; 0: Enter is a key
  int32_t editor_epoch = 0;
};

// The WM_CHAR value the IME side hands the window's own procedure, in place
// of a text event, so that gesher::Window queues it (as a kChar event) in
// order with the window's key events; the connection then takes the text
// event itself from the IME side. No character has this value.
constexpr uint32_t kTextEventMarker = 0x7fff0001u;

// Whether the KeyEvent for Windows virtual key `vk` (scan code `scan`, with
// the extended bit 0x100) with `modifiers` (gesher::WindowEvent's: 1 shift,
// 2 ctrl, 4 alt, 8 caps lock) types `ch` itself: Generic.kcm gives that
// character for its key code and meta state (keymap.cc). When it does not (a
// Hebrew or AZERTY key, AltGr), the character is text for the editor instead.
bool KeyEventTypes(int32_t vk, int32_t scan, int32_t modifiers, uint32_t ch);

}  // namespace input
}  // namespace gesher
