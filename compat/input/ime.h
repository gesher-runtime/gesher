// Gesher: a Gesher window's input method side (docs/input.md, "Text input";
// ADR 0039). Kept apart from the AOSP headers, like win32.h: ime.cc includes
// windows.h and imm32.
//
// ImeWindow subclasses the window's procedure (on the window's own thread
// every message still reaches gesher::Window, except those it takes):
//  - WM_KEYDOWN: the character the key types under the thread's keyboard
//    layout is already queued (TranslateMessage ran), so it is peeked. While an
//    editor has the focus, a key whose KeyEvent would type something else (a
//    Hebrew, Arabic or AZERTY key, AltGr, a dead key) is the input method's:
//    the key is not passed on and its WM_CHAR becomes text. A key whose
//    KeyEvent types its character keeps it, and that WM_CHAR is dropped.
//    VK_PACKET (SendInput's Unicode input) and VK_PROCESSKEY (a key an IME
//    took) are always the input method's. Enter, when the editor has an
//    action, is the action.
//  - WM_CHAR, WM_UNICHAR, WM_IME_CHAR: text (surrogate pairs joined).
//  - WM_IME_SETCONTEXT/STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION: the
//    composition is the editor's (inline, no composition window): its string,
//    caret and target clause, and the result string.
//  - WM_IME_REQUEST IMR_QUERYCHARPOSITION: the caret.
// Each text event is handed to the window's own procedure as a WM_CHAR of
// kTextEventMarker, which gesher::Window queues in order with the keys; the
// connection takes the event with TakeTextEvent when it reads the marker.
// Without an editor the window has no input context (ImmAssociateContextEx):
// keys are keys, IMEs stay out.
#pragma once

#include <stdint.h>

#include <memory>

#include "text_input.h"
#include "text_document.h"

namespace gesher {
namespace input {

class TextService;

class ImeWindow {
 public:
  // Subclasses `hwnd`'s procedure (a Gesher window of this process). Null if
  // the window is gone.
  static std::unique_ptr<ImeWindow> Attach(void* hwnd);
  // Restores the procedure, on the window's thread.
  ~ImeWindow();

  ImeWindow(const ImeWindow&) = delete;
  ImeWindow& operator=(const ImeWindow&) = delete;

  // The next text event; false when there is none. Any thread.
  bool TakeTextEvent(TextEvent* out);
  // What the focused editor takes; applied on the window's thread.
  void SetTextInput(const TextInputState& state);
  void SetDocument(TextDocument document);
  int32_t RunTextServiceTest();
  // Native fixture inspection only, on the window thread. A weak reference
  // lets a COM sink prove ownership during reentrant detach without hiding it.
  std::weak_ptr<TextService> TextServiceForTest();
  // The editor's caret in client pixels: the system caret (screen readers,
  // magnifiers, IMEs use it), the composition and candidate windows'
  // position, IMR_QUERYCHARPOSITION's answer. Applied on the window's thread.
  void SetCaret(int32_t x, int32_t y, int32_t width, int32_t height);
  // Ends the IME's composition: its string becomes the result (complete) or
  // goes (cancel). Applied on the window's thread.
  void CompleteComposition(bool cancel);
  // Shows or hides Windows' touch keyboard for the window, on its thread;
  // waits for the answer (touch_keyboard.h's).
  int32_t ShowTouchKeyboard(bool show);

  // The window procedure's side (ime.cc): the state both sides share and the
  // window thread's own. Left behind (never freed) when the procedure cannot
  // be restored on the window's thread, so that the window never sees it gone.
  struct Impl;

 private:
  friend struct TextServiceProbeAccess;
  explicit ImeWindow(void* hwnd, Impl* impl) : hwnd_(hwnd), impl_(impl) {}
  void Post(int op);

  void* hwnd_;
  Impl* impl_;
};

}  // namespace input
}  // namespace gesher
