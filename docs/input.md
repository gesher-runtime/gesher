# Input (roadmap phase 3)

Goal: a Win32 window's mouse, keyboard, wheel, touch and pen input delivered
to an Android window as `MotionEvent`s and `KeyEvent`s through the real
InputChannel path, so ViewRootImpl's `WindowInputEventReceiver` consumes it as
on a device. Work happens on branch `wip/input`; ADR 0027 has the decisions.

## How it fits together

```
 Win32 window thread (compat/hwui/window)        connection thread (compat/input)            app Looper
 WM_MOUSEMOVE/LBUTTON*/RBUTTON*/MBUTTON*  ──┐
 WM_MOUSEWHEEL/MOUSEHWHEEL/MOUSELEAVE       ├─ WindowEvent queue ─► Translator ─► InputPublisher ══ InputChannel ══► InputConsumer
 WM_POINTER* (touch, pen)                   │   (+ eventfd)          (translate.cc)  (server end)   (socketpair)      (InputEventReceiver)
 WM_KEYDOWN/UP, WM_SYSKEY*, WM_CHAR       ──┘                        ◄── finished signals ──────────────────────── finishInputEvent
```

- **libinput** (`cmake/aosp/input.cmake`, target `android_input`):
  frameworks/native/libs/input's Input.cpp, InputTransport.cpp (InputChannel,
  InputPublisher), InputConsumer.cpp, InputDevice, InputEventLabels,
  KeyCharacterMap, KeyLayoutMap, Keyboard, PrintTools, PropertyMap and
  VelocityTracker, with the AIDL types they use (IInputConstants,
  InputChannelCore, InputConfig, MotionEventFlag, PointerCaptureMode,
  PointerIconType, through `aidl_cpp`) and `com_android_input_flags.h`
  (`tools/gen/aconfig_header.py`, values in `config/aconfig/input.values`).
  `compat/input/libinput`: `input/Input.h` includes the AIDL constants that
  libinput's Input.h includes only under `__linux__`; `rust/cxx.h` and
  `InputVerifier.cc` replace the Rust verifier (it accepts every stream; it
  runs only when `enable_outbound_event_verification` or a log tag asks);
  `evdev/` has the evdev event types InputEventLabels.cpp names (Windows has
  no evdev). Not built: MotionPredictor (TFLite), InputFlags, the cxx bridge.
- **InputChannel over gesher-base**: `openInputChannelPair` makes a
  `socketpair(AF_UNIX, SOCK_SEQPACKET)`, which gesher-base
  (`compat/posix/src/socket.cc`) makes a connected loopback UDP pair:
  message boundaries and order are kept; a full buffer drops instead of
  blocking, and a closed peer is no hangup (the connection bounds what is
  unfinished, below).
- **The framework's natives** (`input_jni`): core/jni's
  `android_view_InputChannel`, `_InputEventReceiver`, `_InputEventSender`,
  `_KeyEvent`, `_MotionEvent`, `_KeyCharacterMap` and `_VelocityTracker`,
  copied into the build tree with System V JNI tables
  (`compat/input/gen_jni_sources.py`, tools/jni-sysv-patch.py's rewrite).
  `compat/input/input_jni.cc`'s `register_gesher_input` registers them from
  libandroid_runtime's table right after hwui's, so they replace the attach
  stand-ins (`attach_standins.cc`, which still lists them; registered first,
  replaced here), then gesher-app's `WindowInput` and `VirtualKeyboard`.
- **The translation** (`compat/input/translate.{h,cc}`):
  - Mouse (`SOURCE_MOUSE`, device 1, tool MOUSE, cursor position set):
    move without buttons is HOVER_ENTER then HOVER_MOVE; WM_MOUSELEAVE is
    HOVER_EXIT; the first button down is HOVER_EXIT (if hovering), DOWN and
    BUTTON_PRESS; further buttons BUTTON_PRESS; releases BUTTON_RELEASE and,
    for the last one, UP; moves with buttons MOVE. Buttons: left PRIMARY,
    right SECONDARY, middle TERTIARY; pressure 1 while pressed. The wheel is
    SCROLL with `AXIS_VSCROLL` (notches, + away from the user) and
    `AXIS_HSCROLL` (+ right, WM_MOUSEHWHEEL). A move at the same place is
    dropped.
  - Touch (`SOURCE_TOUCHSCREEN`, device 2, tool FINGER) from WM_POINTER*
    for `PT_TOUCH` (a pointer Windows does not know, as in a posted message,
    is taken as a finger); pen is `SOURCE_STYLUS|SOURCE_TOUCHSCREEN`, device
    3, tool STYLUS. Android pointer ids are the lowest free ones; DOWN,
    POINTER_DOWN/UP with the pointer's index, MOVE with every pointer, UP;
    WM_POINTERCAPTURECHANGED is CANCEL. Hovering pointers are ignored.
  - Keys (`SOURCE_KEYBOARD`, device -1 = `KeyCharacterMap.VIRTUAL_KEYBOARD`,
    `FLAG_FROM_SYSTEM`): virtual-key code to Android key code
    (`KeyCodeFromVirtualKey`: letters, digits, F1-F12, the keypad, the
    navigation block, modifiers left and right by the scan code's extended
    bit or 0x36, the US layout's punctuation by position, media and browser
    keys), the Windows scan code to the Linux key code
    (`LinuxScanCode`), repeat counts from the message's previous-state bit,
    the down time kept for the repeats and the UP. WM_CHAR is not a key
    event (it goes to the window-event callback).
  - Meta state from the window's modifiers (GetKeyState on the window
    thread): shift, ctrl and alt as `META_*_ON | META_*_LEFT_ON`, caps lock.
  - Coordinates: client pixels (the window is per-monitor DPI aware), times
    `scale` plus `offset`; transform and raw transform are the identity.
  - Event times: `SYSTEM_TIME_MONOTONIC`, dated back by the message's age.
  - Focus: WM_SETFOCUS/KILLFOCUS are focus events (`publishFocusEvent`).
- **The connection** (`compat/input/connection.{h,cc}`): one thread per
  attached window: it polls the window's eventfd, the channel and a wake
  eventfd; translates; publishes with InputPublisher; reads the finished
  signals (`receiveConsumerResponse`); keeps at most `max_in_flight` (32)
  events unfinished, coalescing a waiting MOVE/HOVER_MOVE with the next; a
  send the channel refuses is retried.
- **Generic.kcm** (`compat/input/keychars`, frameworks/base/data/keyboards
  at android-16.0.0_r4) is built in; `$ANDROID_ROOT/usr/keychars/Generic.kcm`
  is used instead when present. `com.gesher.app.input.VirtualKeyboard` loads
  it as a `KeyCharacterMap`, builds the virtual keyboard's `InputDevice`, and
  `VirtualKeyboard.inputManager()` is an `IInputManager` (an
  `IInputManager.Default`) that serves it: `getInputDeviceIds`,
  `getInputDevice`, `hasKeys`.

## For the window session (the C API)

`compat/input/include/gesher_input.h` (C, MS ABI; link `gesher_input`):

```c
gesher_input_connection* gesher_input_attach(void* hwnd, int channel_fd, const char* name,
                                             const struct gesher_input_options* options);
void gesher_input_detach(gesher_input_connection* connection);
void gesher_input_inject(gesher_input_connection* connection, const void* window_event);
void gesher_input_get_stats(gesher_input_connection* connection, struct gesher_input_stats* out);
```

`hwnd` is the Gesher window's HWND (`gesher::Window::hwnd()`,
`NativeWindow.getHwnd()`); `channel_fd` the server end's fd
(`InputChannel::getFd()`, duplicated). Options: `scale`, `offset_x/y` (the
Android window's frame in the client area), `display_id`, `max_in_flight`,
`initial_focus`, and `on_window_event(cookie, const gesher::WindowEvent*)`
for what is not input (resize, close, characters, focus), on the
connection's thread. Detach before the window is destroyed. In C++,
`gesher::input::Connection` (connection.h) takes a `gesher::Window*` and a
`std::shared_ptr<android::InputChannel>`.

From Java (gesher-app), `com.gesher.app.input.WindowInput`:

```java
// IWindowSession.addToDisplay, after creating the window's NativeWindow:
InputChannel[] pair = InputChannel.openInputChannelPair(attrs.getTitle().toString());
pair[1].copyTo(outInputChannel);               // the app's end (ViewRootImpl)
WindowInput.Options o = new WindowInput.Options();
o.offsetX = ...; o.offsetY = ...;              // where the frame is in the client area
o.initialFocus = true;                         // until activation sends WM_SETFOCUS
o.listener = (type, a, b) -> { /* EVENT_RESIZE (a=w, b=h), EVENT_CLOSE, ... */ };
WindowInput input = WindowInput.attach(nativeWindow.getHwnd(), pair[0], o);
// ... remove(): input.close(); pair[0].dispose(); then the window.
```

ViewRootImpl then gets its events in `WindowInputEventReceiver` (do not set
`INPUT_FEATURE_NO_INPUT_CHANNEL`). Register `VirtualKeyboard.inputManager()`
as the "input" service (`Services.create`: `StandIn.of(IInputManager.class,
new VirtualKeyboard.InputManager())`) so that `KeyCharacterMap.load`,
`KeyEvent.getUnicodeChar()` and `VelocityTracker.obtain()` (which asks
InputManagerGlobal for its strategy and throws a NullPointerException
without an input service) work in apps.

## Status

### Done and CI-verified

(see below; filled in from CI)

### Stand-ins and gaps

- Text input: characters a key map cannot produce (other layouts, IMEs)
  need an InputConnection path; WM_CHAR only reaches the callback.
- One process: an InputChannel's fd cannot cross processes yet (gesher-base
  does not duplicate sockets into another process).
- SOCK_SEQPACKET as UDP: no hangup on close; a full buffer drops (bounded by
  `max_in_flight`).
- Not built: MotionPredictor, InputFlags, the Rust InputVerifier,
  InputQueue (NativeActivity's input), InputDevice natives
  (`android_view_InputDevice` is not registered), touchpad gestures,
  pointer capture, drag events, `WM_POINTER` for the mouse
  (EnableMouseInPointer is off).
- Event ids are a counter per connection; no HMAC (`INVALID_HMAC`), so
  `InputManager.verifyInputEvent` cannot verify Gesher's events.
- The window's modifiers are left-hand only (`META_SHIFT_LEFT_ON`...) and
  have no Meta (Windows key) or Num Lock state.

### CI runs
