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

ci run 40038368343 (commit 40d5053), windows-2022 and windows-2025, from
`tests/windows/tracks/33-input.ps1`; every earlier check still passes
(services, graphics, native libraries, the installer):

- `PASS  input: Win32 messages to MotionEvent/KeyEvent over an InputChannel
  (InputPublisher/InputConsumer)` (`probes/input/input_probe.cc`, no Java):
  the translation (buttons, wheel, two fingers, key repeats, the virtual-key
  and scan code tables, meta state, scale and offset); Generic.kcm built in
  (FULL; a, A, !); `InputChannel::openInputChannelPair` over gesher-base's
  socketpair; `gesher_input_attach` on a real Gesher window (and refused for
  another window); WM_MOUSEMOVE/LBUTTONDOWN/MOUSEMOVE/LBUTTONUP/MOUSEWHEEL
  and WM_KEYDOWN (twice, the second a repeat)/KEYUP posted to the HWND read
  back with libinput's InputConsumer as HOVER_ENTER, DOWN, BUTTON_PRESS,
  MOVE, BUTTON_RELEASE, UP, SCROLL and KEYCODE_A (scan 30, repeat 0 then 1);
  injected touch as DOWN/MOVE/UP on SOURCE_TOUCHSCREEN; every finished
  signal read by the connection; injected shift+A with META_SHIFT_ON.
- `PASS  dalvikvm: input: Win32 window messages to a Java InputEventReceiver
  (MotionEvent, KeyEvent, Generic.kcm)` (`tests/dex-graphics/gesher/InputTest.java`,
  framework.jar and gesher-app.jar), with its own checks:
  `KeyEvent.keyCodeToString/keyCodeFromString (libinput's labels)`;
  `MotionEvent: obtain, offsetLocation, transform(Matrix), two pointers, Parcel`;
  `VelocityTracker: 10 px every 10 ms is 999.99963 px/s`;
  `KeyCharacterMap (Generic.kcm): FULL; a, A, !, label Z; getEvents("Hi"); Parcel`;
  `KeyEvent.getUnicodeChar through KeyCharacterMap.load(VIRTUAL_KEYBOARD)`;
  `InputChannel.openInputChannelPair`;
  `InputEventSender to InputEventReceiver on a Looper: a key and a motion,
  finished as handled [11:true, 12:true]`;
  `PostMessage: mouse, wheel and key messages to the Gesher window`;
  `the window's events reached the receiver`;
  `the initial focus event: onFocusEvent(true)`;
  `WM_MOUSEMOVE without buttons: ACTION_HOVER_ENTER at (10, 20), SOURCE_MOUSE`;
  `left and right buttons: DOWN, BUTTON_PRESS, MOVE, BUTTON_PRESS (secondary),
  BUTTON_RELEASE x2, UP (window pixels, SOURCE_MOUSE)`;
  `WM_MOUSEWHEEL / WM_MOUSEHWHEEL: ACTION_SCROLL, AXIS_VSCROLL -1, AXIS_HSCROLL 2`;
  `WM_KEYDOWN/KEYUP: KEYCODE_A (scan 30, repeat 0 then 1, getUnicodeChar 'a'),
  keypad Enter (KEYCODE_NUMPAD_ENTER, scan 96)`;
  `touch: DOWN, MOVE, UP (SOURCE_TOUCHSCREEN, finger)`;
  `meta state: shift+A (isShiftPressed, 'A'), ctrl+click (META_CTRL_ON)`;
  `two fingers: DOWN, POINTER_DOWN (index 1), MOVE, POINTER_UP (index 0), UP`;
  `finished signals: the connection saw every event finished as handled, none dropped`.
- With the real natives, ViewRootImpl's `WindowInputEventReceiver` would
  throw on the stand-in session's uninitialized channel; the generated
  `android_view_InputEventReceiver.cpp` gives such a receiver a channel
  nothing writes to (`gen_jni_sources.py` EDITS) and the services checks pass
  again (they failed in ci run 40035577063 without it).

- **In an app's window** (ci run 40056527378, both runners and the
  installer; tests/windows/tracks/34-windows.ps1 and the package job,
  WindowChecks.ps1's `UiAppInput`): messages posted to uiapp.apk's window
  reach its views through ViewRootImpl: a click runs the Button's
  `onClick`, WM_KEYDOWN/UP (shift for capitals) type "Gesher" into the
  focused EditText and a WM_CHAR U+05D0 is inserted, WM_MOUSEWHEEL scrolls
  a ScrollView (`UiActivity scrolled to 192`): `PASS  gesher-app: input
  reaches uiapp.apk's views in its Win32 window: ...` (single process and
  the Gesher device's app process) and `PASS  installer: input reaches
  uiapp.apk's views on the Gesher device (...)`; docs/graphics.md has the
  details. The `input` service (`VirtualKeyboard.InputManager`) is
  registered in-process (`Services.create`) and by the system server
  (generated `IInputManagerStub`), with `content` and `audio` stand-ins
  that typing and clicking need.

### For NativeActivity (native-libs track)

A NativeActivity's window attaches the same way as an app window: open a
pair (`InputChannel.openInputChannelPair`), `WindowInput.attach(hwnd,
pair[0], options)` with `initialFocus` and a listener (resize, close,
characters, focus; on the connection's thread), and read `pair[1]` with an
`InputEventReceiver` on the app's Looper (or hand it to an `InputQueue` for
`AInputQueue`); `close()` before the window goes. In C++ the same is
`gesher_input_attach(hwnd, fd, name, &options)` (gesher_input.h). WindowInput
holds no state of the window session's, so it is usable from there as is.

### Stand-ins and gaps

- WM_POINTER* touch is exercised only as an INFO line: Windows does not
  post WM_POINTER messages (PostMessage fails), so the checks inject the
  WindowEvents the window's handler queues; the probe also taps the shown
  window through `InjectSyntheticPointerInput` (`probes/input/touch_inject.cc`),
  which needs an interactive desktop. On both runners (ci run 40040115231):
  `INFO  WM_POINTER touch through InjectSyntheticPointerInput: DOWN at (50, 60),
  MOVE, UP on SOURCE_TOUCHSCREEN`. Pen (PT_PEN) is untested.
- App windows use it (wip/graphics, docs/graphics.md "Input in an app's
  window"): `WindowSession` opens the pair in `addToDisplay` and attaches
  WindowInput when the Win32 window is created; verified with uiapp.apk in
  a single process, in an app process of the Gesher device and with the
  installed gesher-app (ci run 40056527378, below).

- Text input: WM_CHARs Generic.kcm cannot type reach the focused view as
  `ACTION_MULTIPLE` KeyEvents (the window session, above); with a non-US
  keyboard layout a key then types both its US character (its KeyEvent)
  and the layout's (its WM_CHAR). No InputConnection (IME) path.
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

- ci 40025490132: libinput: `5ms` without `std::chrono_literals`.
- ci 40026819332: `AMOTION_EVENT_INVALID_CURSOR_POSITION` is in `android::`.
- ci 40027947370: InputEventReceiver needs utils/Trace.h's `ATRACE_*`.
- ci 40030684867: the probe's `<poll.h>` clashes with winsock2.h from utils/FileMap.h.
- ci 40033016380: `GetReferent` (core_jni_helpers.cpp) was not built.
- ci 40035577063: built; the probe passed except posting WM_POINTER (Windows
  refuses); InputTest disposed a receiver off its Looper; every services
  check failed: ViewRootImpl's receiver on the stand-in session's
  uninitialized InputChannel.
- ci 40038368343: green on both runners and the installer.
- ci 40039426375: the probe's synthetic touch (a name clash).
- ci 40040115231: green; real WM_POINTER touch from a synthetic pointer
  device reaches the consumer on both runners (INFO).
