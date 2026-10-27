// Gesher: a top-level Win32 window an Android window is shown in
// (docs/graphics.md, "Window").
//
// Each window has a thread of its own that creates the HWND and runs its
// message loop, so the window stays responsive whatever the app's threads do.
// The app draws into a back buffer in system memory (32-bit BGRA, top-down,
// the layout of Android's PIXEL_FORMAT_BGRA_8888) between Lock() and
// UnlockAndPost(); WM_PAINT and WM_PRINTCLIENT copy it to the window, so
// PrintWindow() reads what was drawn even when no desktop shows the window
// (CI). Input arrives as WindowEvents in a queue; an eventfd (gesher-base) is
// readable while the queue is not empty, so a Looper can watch it.
#pragma once

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace gesher {

class InputPane;
using InputPaneFactory = void* (*)();

struct WindowEvent {
  enum Type : int32_t {
    kMouseDown = 1,  // x, y, button
    kMouseUp = 2,    // x, y, button
    kMouseMove = 3,  // x, y, buttons held (button)
    kWheel = 4,      // x, y, delta (in notches, + is away from the user)
    kKeyDown = 5,    // key (Windows virtual-key code), scan, repeat
    kKeyUp = 6,      // key, scan
    kChar = 7,       // ch (UTF-16 code unit)
    kResize = 8,     // width, height (client area, pixels)
    kFocus = 9,      // button: 1 gained, 0 lost
    kClose = 10,     // the user asked to close the window
    // Input track (docs/input.md): touch and pen contacts (WM_POINTER*),
    // the pointer leaving the client area (WM_MOUSELEAVE).
    kTouchDown = 11,   // x, y, pointer, tool
    kTouchMove = 12,   // x, y, pointer, tool
    kTouchUp = 13,     // x, y, pointer, tool
    kTouchCancel = 14, // pointer, tool
    kMouseLeave = 15,
    // Lifecycle track (docs/lifecycle.md): the window was minimized (button
    // 0) or restored from it (button 1); a press outside a window that
    // watches outside touches (FLAG_WATCH_OUTSIDE_TOUCH: Android's
    // ACTION_OUTSIDE), at x, y of this window's client area, tool 0 for the
    // mouse, else the touch's.
    kVisibility = 16,
    kOutside = 17,
    kActivated = 18,  // top-level activation, independent of keyboard-focus transitions
    // windows3 track (docs/windows3.md): the part of the client area the
    // Windows touch keyboard (IFrameworkInputPane) covers: rows button (top)
    // to modifiers (bottom) of the client area; button -1 when it covers
    // nothing (hidden, or beside the window); -2 means a visible floating
    // pane with no Android edge inset.
    kInputPane = 30,
  };
  int32_t type = 0;
  float x = 0, y = 0;
  int32_t button = 0;  // 1 left, 2 right, 4 middle (a mask for kMouseMove)
  int32_t key = 0;
  int32_t scan = 0;
  int32_t repeat = 0;
  int32_t modifiers = 0;  // 1 shift, 2 ctrl, 4 alt, 8 caps lock
  float delta = 0;
  uint32_t ch = 0;
  int32_t width = 0, height = 0;
  int64_t time_ms = 0;  // GetMessageTime()
  float delta_x = 0;    // kWheel from WM_MOUSEHWHEEL: notches, + is to the right
  int32_t pointer = 0;  // kTouch*: the Windows pointer id
  int32_t tool = 0;     // kTouch*: 1 finger, 2 pen
};

// The window's private messages (WM_APP is its own destruction).
constexpr unsigned kMsgPresentLayered = 0x8000 + 0x40;  // WM_APP + 0x40: UpdateLayeredWindow
constexpr unsigned kMsgActivate = 0x8000 + 0x41;        // bring the window to the front
constexpr unsigned kMsgApplyStyle = 0x8000 + 0x42;      // set the popup's extended style
// The touch keyboard covers screen rows wparam to lparam (0, 0: hidden), as
// IFrameworkInputPane reports it: what the window's input pane handler posts,
// and what tests post to stand for a keyboard (windows3 track).
constexpr unsigned kMsgInputPane = 0x8000 + 0x43;
// The COM callback changed its complete screen rectangle, held by InputPane.
constexpr unsigned kMsgInputPaneChanged = 0x8000 + 0x44;

class Window {
 public:
  // Creates the window (hidden) with a client area of width x height pixels.
  // Returns null if Windows refuses.
  static Window* Create(const std::string& title_utf8, int width, int height,
                        InputPaneFactory input_pane_factory = nullptr);

  ~Window();  // destroys the window and joins its thread

  void Show();
  void Hide();
  // Minimizes the window without activating anything (an app's
  // Activity.moveTaskToBack: its taskbar button stays, restoring it brings
  // the task back; windows3 track).
  void Minimize();
  // Shows the window off the desktop (left of the virtual screen, no taskbar
  // button, not activated), for tests that read it back with PrintWindow. It
  // stays there whatever resizes it (WM_WINDOWPOSCHANGING): a window partly
  // on the desktop is drawn only where it is on it, and PrintWindow reads the
  // rest stale (the parts outside the virtual screen are never painted).
  void ShowOffscreen();
  void SetTitle(const std::string& title_utf8);
  // Lets the user resize (and maximize) the window, or not.
  void SetResizable(bool resizable);
  // Resizes the window so its client area is width x height pixels.
  void SetClientSize(int width, int height);
  // The smallest client area the user can resize the window to (pixels; 0:
  // no minimum). SetWindowPos keeps to it too.
  void SetMinClientSize(int width, int height);
  // The window's icon (title bar, taskbar, Alt+Tab) from an .ico file; false
  // when it cannot be read.
  bool SetIcon(const std::string& ico_path_utf8);
  // The System.AppUserModel.ID given to the window when it was created (the
  // process's; empty when it has none).
  const std::string& app_user_model_id() const { return app_user_model_id_; }

  // The back buffer, locked for drawing: BGRA pixels, `*stride` pixels a row,
  // the size the window had when it was locked (*width x *height).
  uint8_t* Lock(int* width, int* height, int* stride);
  // Unlocks the back buffer and has the window repaint from it.
  void UnlockAndPost();
  // Unlocks without repainting.
  void Unlock();

  int width() const;
  int height() const;
  void* hwnd() const { return hwnd_; }
  // Readable (gesher-base eventfd) while events are queued.
  int event_fd() const { return event_fd_; }
  // Takes the next event; false when there is none.
  bool PollEvent(WindowEvent* event);
  // Waits up to timeout_ms for an event; false on timeout.
  bool WaitEvent(WindowEvent* event, int timeout_ms);
  // Frames posted so far (UnlockAndPost calls).
  uint64_t frames() const { return frames_; }

  // What the window shows of its client area, read back with PrintWindow
  // (which repaints into a memory DC: no visible desktop needed). BGRA pixels,
  // top-down, *width x *height. False if Windows refuses.
  bool PrintClient(std::vector<uint32_t>* pixels, int* width, int* height);
  // A copy of the back buffer (BGRA, top-down).
  void ReadBackBuffer(std::vector<uint32_t>* pixels, int* width, int* height);

  // For the window procedure.
  intptr_t HandleMessage(unsigned msg, uintptr_t wparam, intptr_t lparam);

  // ---- Popups (gesher_popup.cc; docs/graphics.md, "Dialogs, menus and other
  // popups"; ADR 0040) ----

  // Creates a popup window (hidden, no frame, no taskbar button) owned by
  // `owner` (null: no owner) whose client area is width x height pixels.
  // Windows keeps an owned window above its owner and hides it with it.
  static Window* CreatePopup(const std::string& title_utf8, int width, int height,
                             Window* owner);
  // Places the popup's client area at (x, y) of `anchor`'s client area
  // (screen coordinates when anchor is null), width x height pixels. The
  // popup follows the anchor when it moves.
  void SetPopupBounds(Window* anchor, int x, int y, int width, int height);
  // Shows the popup where SetPopupBounds put it, activated or not.
  void ShowPopup(bool activate);
  // What the CPU path presents is shown with per-pixel alpha (a layered
  // window, UpdateLayeredWindow): transparent parts show what is behind.
  void SetTranslucent(bool translucent);
  // Never activated by a click (WS_EX_NOACTIVATE); with `passthrough` also
  // never hit (WS_EX_TRANSPARENT: clicks go to what is below).
  void SetNoActivate(bool no_activate, bool passthrough);
  // Raises the window to the top of its owner's popups (not activated).
  void Raise();
  // Activates the window (its thread brings it to the foreground).
  void Activate();
  // Dims what the window shows by `amount` (0: none, 1: black), as Android's
  // dim layer behind a dialog (FLAG_DIM_BEHIND) does.
  void SetDim(float amount);
  // Where the window's input goes instead of its own queue (null: its own):
  // pointer events (mouse, wheel, touch; a gesture stays with the window its
  // first contact went to), keys and characters. `outside`: windows that
  // watch touches outside them (FLAG_WATCH_OUTSIDE_TOUCH), sent a kOutside
  // event where a gesture starts (Android's ACTION_OUTSIDE). Coordinates are translated through the
  // screen. A window that goes away is dropped from every route.
  void SetInputRoute(Window* pointer, Window* keys, const std::vector<Window*>& outside);
  // The client area in screen coordinates: {left, top, right, bottom}.
  void ScreenBounds(int out[4]) const;
  // Moves the window so its client area's origin is at (x, y) on the screen.
  void SetScreenPosition(int x, int y);
  bool is_popup() const { return popup_; }
  // SetTranslucent(true) was asked: what is presented is composed with alpha.
  bool translucent() const { return translucent_; }
  // Lifecycle track (docs/lifecycle.md): presents a frame drawn elsewhere (SkiaGL's
  // read-back of a translucent popup): `argb` premultiplied 0xAARRGGBB, rows
  // top-down, width x height (cropped or padded to the client area).
  void PresentPixels(const uint32_t* argb, int width, int height);

 private:
  Window() = default;
  // Starts `window`'s thread, which creates its HWND; null (and `window`
  // deleted) if Windows refuses.
  static Window* Start(Window* window, const std::string& title_utf8, int width, int height);
  void Run(std::wstring title, int width, int height);
  // Queues `event` for this window, or where its input route sends it.
  void Queue(const WindowEvent& event);
  // Queues `event` in this window's own queue.
  void QueueOwn(const WindowEvent& event);
  // gesher_popup.cc: whether the route took `event`.
  bool RouteEvent(const WindowEvent& event);
  // gesher_popup.cc: popup messages; true when `*result` answers `msg`.
  bool HandlePopupMessage(unsigned msg, uintptr_t wparam, intptr_t lparam, intptr_t* result);
  // gesher_popup.cc: the window goes; nothing may route to it or anchor to it.
  void Unregister();
  // gesher_popup.cc: puts anchored popups where they belong (the anchor moved).
  void MoveAnchoredPopups();
  // gesher_popup.cc: the extended style for no_activate_, passthrough_ and
  // layered_ (the window's thread).
  void ApplyExStyle();
  // gesher_popup.cc: UpdateLayeredWindow from the back buffer (the window's thread).
  void PresentLayered();
  // gesher_popup.cc: GDI pixels of the buffer, dimmed (with buffer_mutex_ held).
  const uint8_t* DimmedPixels();
  void Paint(void* hdc);
  void ResizeBuffer(int width, int height);  // with mutex_ held
  // The window's thread: has the touch keyboard's IFrameworkInputPane report
  // to the window (top-level windows), and stops it.
  void AdviseInputPane();
  void UnadviseInputPane();
  // The touch keyboard is at this screen rectangle (empty: hidden): queues a
  // kInputPane event when the part of the client area it covers changed.
  void OnInputPane(int left, int top, int right, int bottom);
  // The window moved or was resized: what the keyboard covers again.
  void UpdateInputPane();
  int Modifiers() const;  // the window's thread

  void* hwnd_ = nullptr;
  int event_fd_ = -1;
  void* thread_handle_ = nullptr;
  void* icon_big_ = nullptr;
  void* icon_small_ = nullptr;
  std::string app_user_model_id_;
  uint32_t thread_id_ = 0;

  // Creation handshake.
  std::mutex create_mutex_;
  std::condition_variable create_cv_;
  bool created_ = false;

  // The back buffer.
  mutable std::mutex buffer_mutex_;
  std::vector<uint8_t> pixels_;
  int buffer_width_ = 0, buffer_height_ = 0;
  int client_width_ = 0, client_height_ = 0;  // what WM_SIZE reported
  std::atomic<uint64_t> frames_{0};

  // Input.
  std::mutex event_mutex_;
  std::condition_variable event_cv_;
  std::deque<WindowEvent> events_;
  int buttons_ = 0;
  bool tracking_leave_ = false;  // TrackMouseEvent(TME_LEAVE) is armed
  // Shown off the desktop (ShowOffscreen): resizes keep it there.
  std::atomic<bool> offscreen_{false};
  // Minimized (WM_SIZE SIZE_MINIMIZED), until restored.
  bool minimized_ = false;
  // Modifier keys held as the window's key messages say (shift 1, ctrl 2,
  // alt 4), ORed with GetKeyState's: posted key messages (tests, input
  // injected by other programs) do not change the thread's key state.
  int held_modifiers_ = 0;
  std::atomic<int> min_client_width_{0}, min_client_height_{0};
  // The touch keyboard (AdviseInputPane, the window's thread): the
  // IFrameworkInputPane, its handler and cookie; where the keyboard is on the
  // screen (left, top, right, bottom; empty: hidden) and the rows of the
  // client area it covered when that was last reported.
  std::unique_ptr<InputPane> input_pane_;
  InputPaneFactory input_pane_factory_ = nullptr;  // native COM test source only
  int input_pane_screen_[4] = {0, 0, 0, 0};
  int input_pane_top_ = -2, input_pane_bottom_ = -2;

  // Popups (gesher_popup.cc). The anchor, anchored popups and routes are
  // guarded by the popup registry's mutex.
  bool popup_ = false;
  void* owner_hwnd_ = nullptr;  // CreatePopup's owner, for CreateWindowEx
  Window* anchor_ = nullptr;
  int anchor_x_ = 0, anchor_y_ = 0;  // the popup's place in the anchor's client area
  std::vector<Window*> anchored_;
  Window* route_pointer_ = nullptr;
  Window* route_keys_ = nullptr;
  std::vector<Window*> route_outside_;
  Window* gesture_target_ = nullptr;  // where the pointer gesture under way goes
  bool gesture_active_ = false;
  bool gesture_routed_ = false;  // the gesture goes to gesture_target_ (dropped if gone)
  int gesture_touches_ = 0;
  std::atomic<bool> translucent_{false};
  std::atomic<bool> layered_{false};  // WS_EX_LAYERED set (the window's thread)
  std::atomic<bool> no_activate_{false}, passthrough_{false};
  std::atomic<int> dim_{0};           // 0..255
  std::vector<uint8_t> dimmed_;       // buffer_mutex_
};

}  // namespace gesher
