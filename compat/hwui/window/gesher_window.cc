// Gesher: a top-level Win32 window an Android window is shown in.
// See gesher_window.h.

// The project defines NOGDI for AOSP code; the window paints with GDI.
#undef NOGDI
#include <windows.h>
#include <windowsx.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "gesher_window.h"

namespace gesher {
namespace {

constexpr wchar_t kClassName[] = L"GesherWindow";

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

int Modifiers() {
  int m = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= 2;
  if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
  if (GetKeyState(VK_CAPITAL) & 1) m |= 8;
  return m;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (window != nullptr) {
    LRESULT r = window->HandleMessage(msg, wparam, lparam);
    if (r != -1) return r;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RegisterClassOnce() {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(
      &once,
      [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        // Pixels are pixels: the app lays out with its own density.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        return TRUE;
      },
      nullptr, nullptr);
}

struct StartArgs {
  Window* window;
  std::wstring title;
  int width, height;
};

}  // namespace

Window* Window::Create(const std::string& title_utf8, int width, int height) {
  RegisterClassOnce();
  auto* window = new Window();
  window->event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  {
    std::lock_guard<std::mutex> lock(window->buffer_mutex_);
    window->client_width_ = width;
    window->client_height_ = height;
    window->ResizeBuffer(width, height);
  }
  auto* args = new StartArgs{window, Widen(title_utf8), width, height};
  DWORD tid = 0;
  HANDLE thread = CreateThread(
      nullptr, 0,
      [](LPVOID p) -> DWORD {
        auto* a = static_cast<StartArgs*>(p);
        a->window->Run(std::move(a->title), a->width, a->height);
        delete a;
        return 0;
      },
      args, 0, &tid);
  if (thread == nullptr) {
    delete args;
    delete window;
    return nullptr;
  }
  window->thread_id_ = tid;
  window->thread_handle_ = thread;
  std::unique_lock<std::mutex> lock(window->create_mutex_);
  window->create_cv_.wait(lock, [window] { return window->created_; });
  if (window->hwnd_ == nullptr) {
    lock.unlock();
    delete window;
    return nullptr;
  }
  return window;
}

void Window::Run(std::wstring title, int width, int height) {
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT r = {0, 0, width, height};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
  {
    std::lock_guard<std::mutex> lock(create_mutex_);
    hwnd_ = hwnd;
    created_ = true;
  }
  create_cv_.notify_all();
  if (hwnd == nullptr) return;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

Window::~Window() {
  if (hwnd_ != nullptr) {
    // DestroyWindow must run on the window's thread.
    PostMessageW(static_cast<HWND>(hwnd_), WM_APP, 0, 0);
  }
  if (thread_handle_ != nullptr) {
    WaitForSingleObject(thread_handle_, INFINITE);
    CloseHandle(thread_handle_);
  }
  if (event_fd_ >= 0) close(event_fd_);
}

void Window::Show() {
  ShowWindowAsync(static_cast<HWND>(hwnd_), SW_SHOWNORMAL);
}

void Window::SetTitle(const std::string& title_utf8) {
  // SetWindowTextW from another thread sends WM_SETTEXT to the window's
  // thread and waits; the title is copied there.
  SetWindowTextW(static_cast<HWND>(hwnd_), Widen(title_utf8).c_str());
}

void Window::ResizeBuffer(int width, int height) {
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  if (width == buffer_width_ && height == buffer_height_) return;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0xff);
  // Keep what overlaps, so a repaint before the next frame shows the old one.
  for (int y = 0; y < height && y < buffer_height_; y++) {
    std::memcpy(&pixels[static_cast<size_t>(y) * width * 4],
                &pixels_[static_cast<size_t>(y) * buffer_width_ * 4],
                static_cast<size_t>(width < buffer_width_ ? width : buffer_width_) * 4);
  }
  pixels_.swap(pixels);
  buffer_width_ = width;
  buffer_height_ = height;
}

uint8_t* Window::Lock(int* width, int* height, int* stride) {
  buffer_mutex_.lock();
  ResizeBuffer(client_width_, client_height_);
  *width = buffer_width_;
  *height = buffer_height_;
  *stride = buffer_width_;
  return pixels_.data();
}

void Window::Unlock() {
  buffer_mutex_.unlock();
}

void Window::UnlockAndPost() {
  frames_++;
  buffer_mutex_.unlock();
  InvalidateRect(static_cast<HWND>(hwnd_), nullptr, FALSE);
}

int Window::width() const {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return client_width_;
}

int Window::height() const {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return client_height_;
}

void Window::Paint(void* hdc_ptr) {
  HDC hdc = static_cast<HDC>(hdc_ptr);
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = buffer_width_;
  bmi.bmiHeader.biHeight = -buffer_height_;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  SetDIBitsToDevice(hdc, 0, 0, buffer_width_, buffer_height_, 0, 0, 0, buffer_height_,
                    pixels_.data(), &bmi, DIB_RGB_COLORS);
}

bool Window::PrintClient(std::vector<uint32_t>* out, int* width, int* height) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  RECT rc;
  if (!GetClientRect(hwnd, &rc)) return false;
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w < 1 || h < 1) return false;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp != nullptr) {
    HGDIOBJ old = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
    ok = PrintWindow(hwnd, dc, PW_CLIENTONLY) != 0;
    GdiFlush();
    out->assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + static_cast<size_t>(w) * h);
    SelectObject(dc, old);
    DeleteObject(bmp);
  }
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  *width = w;
  *height = h;
  return ok;
}

void Window::ReadBackBuffer(std::vector<uint32_t>* out, int* width, int* height) {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  const uint32_t* p = reinterpret_cast<const uint32_t*>(pixels_.data());
  out->assign(p, p + static_cast<size_t>(buffer_width_) * buffer_height_);
  *width = buffer_width_;
  *height = buffer_height_;
}

void Window::Queue(const WindowEvent& event) {
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    events_.push_back(event);
  }
  event_cv_.notify_all();
  if (event_fd_ >= 0) eventfd_write(event_fd_, 1);
}

bool Window::PollEvent(WindowEvent* event) {
  std::lock_guard<std::mutex> lock(event_mutex_);
  if (events_.empty()) {
    if (event_fd_ >= 0) {
      eventfd_t ignored;
      eventfd_read(event_fd_, &ignored);  // nonblocking: clears the count
    }
    return false;
  }
  *event = events_.front();
  events_.pop_front();
  return true;
}

bool Window::WaitEvent(WindowEvent* event, int timeout_ms) {
  {
    std::unique_lock<std::mutex> lock(event_mutex_);
    if (!event_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return !events_.empty(); })) {
      return false;
    }
  }
  return PollEvent(event);
}

intptr_t Window::HandleMessage(unsigned msg, uintptr_t wparam, intptr_t lparam) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  WindowEvent e;
  e.time_ms = GetMessageTime();
  switch (msg) {
    case WM_APP:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_CLOSE:
      e.type = WindowEvent::kClose;
      Queue(e);
      return 0;  // the app decides
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      Paint(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:
      Paint(reinterpret_cast<HDC>(wparam));
      return 0;
    case WM_GETMINMAXINFO: {
      // Allow windows larger than the screen (a phone-sized window on a
      // small CI desktop).
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMaxTrackSize.x = 16384;
      info->ptMaxTrackSize.y = 16384;
      return 0;
    }
    case WM_SIZE: {
      if (wparam == SIZE_MINIMIZED) return 0;
      int w = LOWORD(lparam), h = HIWORD(lparam);
      bool changed;
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        changed = w != client_width_ || h != client_height_;
        client_width_ = w;
        client_height_ = h;
      }
      if (changed) {
        e.type = WindowEvent::kResize;
        e.width = w;
        e.height = h;
        Queue(e);
      }
      return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      e.type = WindowEvent::kFocus;
      e.button = msg == WM_SETFOCUS;
      Queue(e);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
      bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
      int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? 1
                   : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2
                                                                    : 4;
      buttons_ = down ? (buttons_ | button) : (buttons_ & ~button);
      if (buttons_ != 0) {
        SetCapture(hwnd);
      } else {
        ReleaseCapture();
      }
      e.type = down ? WindowEvent::kMouseDown : WindowEvent::kMouseUp;
      e.x = static_cast<float>(GET_X_LPARAM(lparam));
      e.y = static_cast<float>(GET_Y_LPARAM(lparam));
      e.button = button;
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    }
    case WM_MOUSEMOVE:
      e.type = WindowEvent::kMouseMove;
      e.x = static_cast<float>(GET_X_LPARAM(lparam));
      e.y = static_cast<float>(GET_Y_LPARAM(lparam));
      e.button = buttons_;
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    case WM_MOUSEWHEEL: {
      POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};  // screen coordinates
      ScreenToClient(hwnd, &p);
      e.type = WindowEvent::kWheel;
      e.x = static_cast<float>(p.x);
      e.y = static_cast<float>(p.y);
      e.delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
      e.type = down ? WindowEvent::kKeyDown : WindowEvent::kKeyUp;
      e.key = static_cast<int32_t>(wparam);
      e.scan = static_cast<int32_t>((lparam >> 16) & 0x1ff);
      e.repeat = down && (lparam & (1 << 30)) ? 1 : 0;
      e.modifiers = Modifiers();
      Queue(e);
      // Alt+F4 and the system menu keep working.
      return msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ? -1 : 0;
    }
    case WM_CHAR:
      e.type = WindowEvent::kChar;
      e.ch = static_cast<uint32_t>(wparam);
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    default:
      return -1;  // DefWindowProc
  }
}

}  // namespace gesher
