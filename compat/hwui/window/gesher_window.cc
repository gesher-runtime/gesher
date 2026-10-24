// Gesher: a top-level Win32 window an Android window is shown in.
// See gesher_window.h.

// The project defines NOGDI for AOSP code; the window paints with GDI. The
// property store (the window's AppUserModelID) needs what
// WIN32_LEAN_AND_MEAN leaves out.
#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>

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

// The modifier bit (WindowEvent::modifiers) of a virtual key, or 0.
int ModifierOfKey(uintptr_t vk) {
  switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return 1;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 2;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return 4;
    default: return 0;
  }
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
        // IDC_* and IDI_* are typed for the ANSI functions without UNICODE.
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        return TRUE;
      },
      nullptr, nullptr);
}

// IID_IPropertyStore and PKEY_AppUserModel_ID (propkey.h), spelled out: not
// every GUID is in llvm-mingw's libuuid.
const IID kIidPropertyStore = {0x886D8EEB, 0x8CF2, 0x4446,
                               {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
const PROPERTYKEY kAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

// shell32's functions, looked up when first used (dalvikvm does not link
// shell32).
void* Shell32(const char* name) {
  static HMODULE module = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module == nullptr ? nullptr : reinterpret_cast<void*>(GetProcAddress(module, name));
}

// The AppUserModelID the process's windows go under on the taskbar: the
// process's explicit one (SetCurrentProcessExplicitAppUserModelID, which app
// processes set from $GESHER_APP_USER_MODEL_ID), else that variable.
std::wstring ProcessAppUserModelId() {
  using GetAppIdFn = HRESULT(WINAPI*)(PWSTR*);
  auto get = reinterpret_cast<GetAppIdFn>(Shell32("GetCurrentProcessExplicitAppUserModelID"));
  PWSTR id = nullptr;
  if (get != nullptr && SUCCEEDED(get(&id)) && id != nullptr) {
    std::wstring out(id);
    CoTaskMemFree(id);
    if (!out.empty()) return out;
  }
  wchar_t env[256];
  DWORD n = GetEnvironmentVariableW(L"GESHER_APP_USER_MODEL_ID", env, 256);
  return n > 0 && n < 256 ? std::wstring(env, n) : std::wstring();
}

// Sets the window's System.AppUserModel.ID (SHGetPropertyStoreForWindow), so
// it groups with the app's Start menu shortcut, as the process's ID does.
HRESULT SetWindowAppUserModelId(HWND hwnd, const std::wstring& id) {
  using GetStoreFn = HRESULT(WINAPI*)(HWND, REFIID, void**);
  auto get = reinterpret_cast<GetStoreFn>(Shell32("SHGetPropertyStoreForWindow"));
  if (get == nullptr) return E_NOTIMPL;
  IPropertyStore* store = nullptr;
  HRESULT hr = get(hwnd, kIidPropertyStore, reinterpret_cast<void**>(&store));
  if (FAILED(hr)) return hr;
  PROPVARIANT value;
  std::memset(&value, 0, sizeof(value));
  size_t bytes = (id.size() + 1) * sizeof(wchar_t);
  value.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
  if (value.pwszVal == nullptr) {
    store->Release();
    return E_OUTOFMEMORY;
  }
  std::memcpy(value.pwszVal, id.c_str(), bytes);
  value.vt = VT_LPWSTR;
  hr = store->SetValue(kAppUserModelId, value);
  if (SUCCEEDED(hr)) hr = store->Commit();
  PropVariantClear(&value);
  store->Release();
  return hr;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

struct StartArgs {
  Window* window;
  std::wstring title;
  int width, height;
};

}  // namespace

int Window::Modifiers() const {
  int m = held_modifiers_;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= 2;
  if (GetKeyState(VK_MENU) & 0x8000) m |= 4;
  if (GetKeyState(VK_CAPITAL) & 1) m |= 8;
  return m;
}

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
  // COM for the window's property store, on the window's own thread.
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT r = {0, 0, width, height};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
  if (hwnd != nullptr) {
    std::wstring id = ProcessAppUserModelId();
    if (!id.empty() && SUCCEEDED(SetWindowAppUserModelId(hwnd, id))) {
      app_user_model_id_ = Narrow(id);
    }
  }
  {
    std::lock_guard<std::mutex> lock(create_mutex_);
    hwnd_ = hwnd;
    created_ = true;
  }
  create_cv_.notify_all();
  if (hwnd != nullptr) {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (SUCCEEDED(com)) CoUninitialize();
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
  if (icon_big_ != nullptr) DestroyIcon(static_cast<HICON>(icon_big_));
  if (icon_small_ != nullptr) DestroyIcon(static_cast<HICON>(icon_small_));
}

bool Window::SetIcon(const std::string& ico_path_utf8) {
  std::wstring path = Widen(ico_path_utf8);
  HICON big = static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXICON),
                                            GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
  HICON little = static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
  if (big == nullptr && little == nullptr) return false;
  HWND hwnd = static_cast<HWND>(hwnd_);
  // WM_SETICON from another thread is sent to the window's thread. The
  // window uses the icons until it is destroyed, so they live as long.
  if (big != nullptr) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
  if (little != nullptr) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little));
  }
  std::lock_guard<std::mutex> lock(create_mutex_);
  if (icon_big_ != nullptr) DestroyIcon(static_cast<HICON>(icon_big_));
  if (icon_small_ != nullptr) DestroyIcon(static_cast<HICON>(icon_small_));
  icon_big_ = big;
  icon_small_ = little;
  return true;
}

void Window::Show() {
  ShowWindowAsync(static_cast<HWND>(hwnd_), SW_SHOWNORMAL);
}

void Window::Hide() {
  ShowWindowAsync(static_cast<HWND>(hwnd_), SW_HIDE);
}

void Window::ShowOffscreen() {
  HWND hwnd = static_cast<HWND>(hwnd_);
  // A tool window (no taskbar button) left of the virtual screen, shown
  // without activation: off the desktop, but drawn and composed like any
  // shown window (PrintWindow reads nothing back from a hidden one).
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                    GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
  RECT r;
  GetWindowRect(hwnd, &r);
  int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 64;
  int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  SetWindowPos(hwnd, HWND_BOTTOM, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

void Window::SetResizable(bool resizable) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR frame = WS_THICKFRAME | WS_MAXIMIZEBOX;
  style = resizable ? (style | frame) : (style & ~frame);
  SetWindowLongPtrW(hwnd, GWL_STYLE, style);
  SetClientSize(width(), height());  // the frame changed; keep the client area
}

void Window::SetClientSize(int width, int height) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  RECT r = {0, 0, width, height};
  AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                           GetDpiForWindow(hwnd));
  // SetWindowPos from another thread waits for the window's thread, which
  // handles WM_SIZE (the new client size) before it returns.
  SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void Window::SetMinClientSize(int width, int height) {
  min_client_width_ = width > 0 ? width : 0;
  min_client_height_ = height > 0 ? height : 0;
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
      // The app's minimum (SetMinClientSize), as a window size.
      const int min_w = min_client_width_, min_h = min_client_height_;
      if (min_w > 0 || min_h > 0) {
        RECT r = {0, 0, min_w, min_h};
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                                 FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                                 GetDpiForWindow(hwnd));
        if (min_w > 0) info->ptMinTrackSize.x = r.right - r.left;
        if (min_h > 0) info->ptMinTrackSize.y = r.bottom - r.top;
      }
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
      if (msg == WM_KILLFOCUS) held_modifiers_ = 0;  // their key-up messages go elsewhere
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
      if (!tracking_leave_) {
        // One WM_MOUSELEAVE when the pointer leaves the client area.
        TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, hwnd, 0};
        tracking_leave_ = TrackMouseEvent(&track) != 0;
      }
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
    case WM_MOUSEHWHEEL: {
      POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};  // screen coordinates
      ScreenToClient(hwnd, &p);
      e.type = WindowEvent::kWheel;
      e.x = static_cast<float>(p.x);
      e.y = static_cast<float>(p.y);
      e.delta_x = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_leave_ = false;
      e.type = WindowEvent::kMouseLeave;
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    // Touch and pen contacts (docs/input.md). The mouse and touchpads keep
    // their mouse messages (EnableMouseInPointer is off). A pointer Windows
    // does not know, as in a posted message, is taken as a finger.
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    case WM_POINTERCAPTURECHANGED: {
      UINT32 id = GET_POINTERID_WPARAM(wparam);
      POINTER_INPUT_TYPE type = PT_TOUCH;
      if (GetPointerType(id, &type) && type != PT_TOUCH && type != PT_PEN) return -1;
      if (msg == WM_POINTERUPDATE && !IS_POINTER_INCONTACT_WPARAM(wparam)) return 0;  // hovering
      if (msg == WM_POINTERCAPTURECHANGED) {
        e.type = WindowEvent::kTouchCancel;
      } else {
        POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};  // screen coordinates
        ScreenToClient(hwnd, &p);
        e.x = static_cast<float>(p.x);
        e.y = static_cast<float>(p.y);
        e.type = msg == WM_POINTERDOWN ? WindowEvent::kTouchDown
                 : msg == WM_POINTERUP ? WindowEvent::kTouchUp
                                       : WindowEvent::kTouchMove;
      }
      e.pointer = static_cast<int32_t>(id);
      e.tool = type == PT_PEN ? 2 : 1;
      e.modifiers = Modifiers();
      Queue(e);
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
      if (int bit = ModifierOfKey(wparam)) {
        held_modifiers_ = down ? (held_modifiers_ | bit) : (held_modifiers_ & ~bit);
      }
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
