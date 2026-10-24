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
#include <string>
#include <vector>

namespace gesher {

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

class Window {
 public:
  // Creates the window (hidden) with a client area of width x height pixels.
  // Returns null if Windows refuses.
  static Window* Create(const std::string& title_utf8, int width, int height);

  ~Window();  // destroys the window and joins its thread

  void Show();
  void Hide();
  // Shows the window off the desktop (left of the virtual screen, no taskbar
  // button, not activated), for tests that read it back with PrintWindow.
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

 private:
  Window() = default;
  void Run(std::wstring title, int width, int height);
  void Queue(const WindowEvent& event);
  void Paint(void* hdc);
  void ResizeBuffer(int width, int height);  // with mutex_ held
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
  // Modifier keys held as the window's key messages say (shift 1, ctrl 2,
  // alt 4), ORed with GetKeyState's: posted key messages (tests, input
  // injected by other programs) do not change the thread's key state.
  int held_modifiers_ = 0;
  std::atomic<int> min_client_width_{0}, min_client_height_{0};
};

}  // namespace gesher
