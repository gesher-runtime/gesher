// Gesher: Android input from a Win32 window (docs/input.md).
//
// A gesher::Window (compat/hwui/window) queues its mouse, keyboard, wheel and
// touch messages as WindowEvents. An input connection reads that queue on a
// thread of its own, translates each event into Android's form (MotionEvent
// for SOURCE_MOUSE / SOURCE_TOUCHSCREEN / SOURCE_STYLUS, KeyEvent with
// Android key codes and meta state, focus events) and publishes it on the
// server end of an InputChannel pair with libinput's InputPublisher. The app
// end is what ViewRootImpl's WindowInputEventReceiver (or any
// android.view.InputEventReceiver) consumes; the connection reads the
// consumer's finished signals and keeps at most `max_in_flight` events
// unfinished, coalescing pointer moves while it waits, as InputDispatcher
// does.
//
// Coordinates are the window's client pixels (the window is per-monitor DPI
// aware: physical pixels), mapped as x * scale + offset_x, y * scale +
// offset_y, so a session can place the Android window's frame in the client
// area.
//
// While a connection is attached it is the only reader of the window's
// event queue: window events that are not input (resize, close, focus,
// characters) go to `on_window_event`, on the connection's thread. Detach
// before the window is destroyed.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


struct gesher_input_options {
  float scale;      // 0 means 1
  float offset_x;   // added after scaling
  float offset_y;
  int32_t display_id;     // Android logical display id (0, the default display)
  int32_t max_in_flight;  // unfinished events at most; 0 means 32
  // Window events that are not input, on the connection's thread: `event`
  // is a gesher::WindowEvent*. May be null.
  void (*on_window_event)(void* cookie, const void* event);
  void* cookie;
  // Sends a focus event (has focus) as soon as the connection starts, for a
  // window that is not shown or not activated (tests, an app's first frame).
  int32_t initial_focus;
};

typedef struct gesher_input_connection gesher_input_connection;

// Attaches the Gesher window whose HWND is `hwnd` (gesher_window_hwnd,
// NativeWindow.getHwnd()) to an InputChannel's server end: `channel_fd` is
// its fd (InputChannel::getFd()), duplicated here, so the caller keeps its
// own. `name` names the channel in logs (may be null). `options` may be null.
// Returns null if `hwnd` is not a Gesher window or the fd cannot be used.
gesher_input_connection* gesher_input_attach(void* hwnd, int channel_fd, const char* name,
                                             const struct gesher_input_options* options);

// Stops the connection's thread and closes its end of the channel. Events
// still queued in the window stay there.
void gesher_input_detach(gesher_input_connection* connection);

// Translates and publishes `event` (a gesher::WindowEvent*) as if the window
// had queued it, on the connection's thread: for sessions that read the
// window's queue themselves, and for tests (synthetic modifiers, touch).
void gesher_input_inject(gesher_input_connection* connection, const void* event);

// Counters: events published, finished signals received, finished with
// handled = true, and events published but not finished yet.
struct gesher_input_stats {
  uint64_t published;
  uint64_t finished;
  uint64_t handled;
  uint64_t in_flight;
  uint64_t dropped;  // send failures (channel full or closed)
};
void gesher_input_get_stats(gesher_input_connection* connection, struct gesher_input_stats* out);

#ifdef __cplusplus
}  // extern "C"
#endif
