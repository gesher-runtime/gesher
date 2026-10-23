# 0027. Input: InputChannel over gesher-base, Win32 messages to Android events

Status: accepted (input track, Phase 3). The coordinator may renumber.

## Context

On Android, InputFlinger reads evdev devices, InputDispatcher picks the
window, and an InputPublisher writes each event into the server end of the
window's InputChannel, a `SOCK_SEQPACKET` socketpair; the app's
ViewRootImpl reads the client end with an InputEventReceiver (libinput's
InputConsumer) on its Looper and answers every event with a finished signal.
Gesher has no evdev and no InputFlinger: each Android window is a Win32
window (ADR 0021) whose thread already queues its mouse and key messages as
`gesher::WindowEvent`s. libinput, which the framework's input natives are
written against, builds for the host but also carries a Rust half (the cxx
bridge, the InputVerifier), TensorFlow Lite (MotionPredictor) and aconfig
flags.

## Decision

- **The real transport.** libinput's Input.cpp, InputTransport.cpp
  (InputChannel, InputPublisher), InputConsumer.cpp, InputDevice,
  KeyCharacterMap, KeyLayoutMap, Keyboard, PropertyMap and VelocityTracker
  are built (`cmake/aosp/input.cmake`), and core/jni's InputChannel,
  InputEventReceiver, InputEventSender, KeyEvent, MotionEvent,
  KeyCharacterMap and VelocityTracker natives replace the attach stand-ins.
  Left out: MotionPredictor (TFLite), InputFlags, the Rust verifier (a C++
  one that accepts every stream; it only runs when a flag or log tag asks).
- **SOCK_SEQPACKET socketpairs are connected UDP pairs in gesher-base.**
  Windows has no `AF_UNIX` datagram or seqpacket sockets; a loopback UDP
  pair keeps message boundaries and order, and a connected UDP socket only
  receives from its peer. What differs from Linux: a full receive buffer
  drops a message instead of blocking the sender (`WOULD_BLOCK`), and a
  closed peer is not reported as a hangup. The publisher side therefore
  bounds what it sends without a finished signal (below).
- **Translation in C++, on a thread per connection** (`compat/input`).
  `Translator` maps WindowEvents to Android events with InputReader's and
  InputDispatcher's conventions: the mouse is `SOURCE_MOUSE` with
  HOVER_ENTER/MOVE/EXIT, DOWN + BUTTON_PRESS, BUTTON_RELEASE + UP, SCROLL
  with `AXIS_VSCROLL`/`AXIS_HSCROLL`, and a cursor position; touch is
  `SOURCE_TOUCHSCREEN` with Android pointer ids assigned lowest-first and
  POINTER_DOWN/UP by index; pen is `SOURCE_STYLUS`; keys get Android key
  codes from virtual-key codes (the scan code's extended bit tells right
  Ctrl/Alt and the keypad's Enter), Linux scan codes, meta state and repeat
  counts, from the virtual keyboard (device -1). A `Connection` reads the
  window's queue (it becomes the queue's only reader; resize, close, focus
  and characters go to a callback), publishes with InputPublisher, reads the
  consumer's finished signals and keeps at most 32 events unfinished,
  coalescing moves while it waits, as InputDispatcher does.
- **Event times** are `SYSTEM_TIME_MONOTONIC` (SystemClock.uptimeMillis's
  clock), dated back by the message's age (`GetMessageTime`), never earlier
  than the previous event.
- **The C API** (`gesher_input.h`: `gesher_input_attach(hwnd, fd, name,
  options)`, `detach`, `inject`, `get_stats`) and its Java face
  (`com.gesher.app.input.WindowInput`) attach a window to an InputChannel's
  server end: a window session opens the pair in `addToDisplay`, gives the
  client end to the app and attaches the server end.
- **System V copies instead of a patch.** The core/jni input sources'
  tables are rewritten into the build tree by `gen_jni_sources.py` (with
  tools/jni-sysv-patch.py's rewrite) rather than added to frameworks/base
  patch 0002: patches are part of the AOSP cache key, and other tracks edit
  that patch.
- **Generic.kcm is built in** (`compat/input/keychars`, a verbatim copy at
  android-16.0.0_r4), and `$ANDROID_ROOT/usr/keychars/Generic.kcm` wins when
  present; aosp.lock's frameworks/base line, part of the host-tools cache
  key, stays as it is. `VirtualKeyboard.inputManager()` is an
  IInputManager that knows that keyboard, for `InputManagerGlobal`
  (KeyCharacterMap.load, KeyEvent.getUnicodeChar, VelocityTracker's
  strategy).

## Consequences

- Text that a key map cannot produce (another keyboard layout, an IME) does
  not come from WM_CHAR; it needs an InputConnection path later. WM_CHAR
  reaches the connection's callback only.
- An InputChannel is in-process for now: passing its fd to another process
  (binder) needs gesher-base to duplicate sockets across processes.
- Hover and leave depend on the cursor; a hidden window may get
  WM_MOUSELEAVE at once, so tests ignore hover events except the first.
