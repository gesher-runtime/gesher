// gesher (surfaces track, ADR 0056): EGL window surfaces on ANativeWindows
// without an HWND. See egl_child_surface.h.
//
// ANGLE is called through gesher_egl_proc (its Microsoft-ABI entry points),
// as code in the executable does; this file never goes through the System V
// thunks apps use.

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include "egl_child_surface.h"

#include <GLES3/gl3.h>

#include <gesher/egl_angle.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gesher::bionic::gl {
namespace {

// --- ANGLE ---------------------------------------------------------------------------------------

template <typename F>
F Proc(const char* name) {
  return reinterpret_cast<F>(gesher_egl_proc(name));
}

struct Angle {
  EGLSurface(EGLAPIENTRY* get_current_surface)(EGLint);
  EGLContext(EGLAPIENTRY* get_current_context)();
  EGLBoolean(EGLAPIENTRY* query_surface)(EGLDisplay, EGLSurface, EGLint, EGLint*);
  EGLBoolean(EGLAPIENTRY* query_context)(EGLDisplay, EGLContext, EGLint, EGLint*);
  void(GL_APIENTRY* get_integerv)(GLenum, GLint*);
  void(GL_APIENTRY* pixel_storei)(GLenum, GLint);
  void(GL_APIENTRY* bind_framebuffer)(GLenum, GLuint);
  void(GL_APIENTRY* bind_buffer)(GLenum, GLuint);
  void(GL_APIENTRY* read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
};

const Angle* GetAngle() {
  static const Angle* angle = [] {
    auto* a = new Angle();
    a->get_current_surface = Proc<decltype(a->get_current_surface)>("eglGetCurrentSurface");
    a->get_current_context = Proc<decltype(a->get_current_context)>("eglGetCurrentContext");
    a->query_surface = Proc<decltype(a->query_surface)>("eglQuerySurface");
    a->query_context = Proc<decltype(a->query_context)>("eglQueryContext");
    a->get_integerv = Proc<decltype(a->get_integerv)>("glGetIntegerv");
    a->pixel_storei = Proc<decltype(a->pixel_storei)>("glPixelStorei");
    a->bind_framebuffer = Proc<decltype(a->bind_framebuffer)>("glBindFramebuffer");
    a->bind_buffer = Proc<decltype(a->bind_buffer)>("glBindBuffer");
    a->read_pixels = Proc<decltype(a->read_pixels)>("glReadPixels");
    if (a->get_current_surface == nullptr || a->get_current_context == nullptr ||
        a->query_surface == nullptr || a->query_context == nullptr || a->get_integerv == nullptr ||
        a->pixel_storei == nullptr || a->bind_framebuffer == nullptr || a->bind_buffer == nullptr ||
        a->read_pixels == nullptr) {
      std::fprintf(stderr, "gesher EGL: child surfaces: ANGLE lacks a function they need\n");
      std::fflush(stderr);
      delete a;
      return static_cast<Angle*>(nullptr);
    }
    return a;
  }();
  return angle;
}

// --- helper windows ------------------------------------------------------------------------------

constexpr wchar_t kHelperClass[] = L"GesherEglChildSurface";

struct Helper {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  HWND hwnd = nullptr;
  int width = 1, height = 1;
};

LRESULT CALLBACK HelperProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_PAINT:
      ValidateRect(hwnd, nullptr);  // never shown; ANGLE's swap chain owns it
      return 0;
    case WM_ERASEBKGND:
      return 1;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

// The helper's thread: creates the window and pumps its messages (DXGI and
// cross-thread SetWindowPos send it messages) until it is destroyed.
void HelperThread(Helper* h) {
  static const bool registered = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = HelperProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHelperClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }();
  HWND hwnd = nullptr;
  if (registered) {
    // A popup has no frame: its client area is its size. Placed off the
    // desktop in case anything ever shows it; never shown here.
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHelperClass,
                           L"Gesher EGL surface", WS_POPUP, -32000, -32000, h->width, h->height,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  {
    std::lock_guard<std::mutex> lock(h->mutex);
    h->hwnd = hwnd;
    h->done = true;
    // Under the lock: the creator deletes `h` once it sees `done`.
    h->cv.notify_all();
  }
  if (hwnd == nullptr) return;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

struct Child {
  ANativeWindow* window = nullptr;
  HWND helper = nullptr;
  std::thread* thread = nullptr;
  int helper_width = 0, helper_height = 0;
  std::chrono::steady_clock::time_point last_post{};
  std::vector<uint32_t> pixels;
};

std::mutex g_mutex;
std::unordered_map<HWND, std::thread*>& Threads() {
  static auto* map = new std::unordered_map<HWND, std::thread*>();
  return *map;
}
std::unordered_map<EGLSurface, Child*>& Children() {
  static auto* map = new std::unordered_map<EGLSurface, Child*>();
  return *map;
}

void WindowSize(ANativeWindow* window, int* w, int* h) {
  *w = ANativeWindow_getWidth(window);
  *h = ANativeWindow_getHeight(window);
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
}

void DestroyHelperWindow(HWND hwnd, std::thread* thread) {
  if (hwnd != nullptr) PostMessageW(hwnd, WM_CLOSE, 0, 0);  // DefWindowProc destroys it
  if (thread != nullptr) {
    if (thread->joinable()) thread->join();
    delete thread;
  }
}

// Android's HAL pixel formats (system/graphics.h).
constexpr int HAL_PIXEL_FORMAT_RGBX_8888 = 2;
constexpr int HAL_PIXEL_FORMAT_RGB_565 = 4;
constexpr int HAL_PIXEL_FORMAT_BGRA_8888 = 5;

// RGBA (R in the low byte) to the buffer's pixel format.
void PutRow(const uint32_t* src, void* dst, int count, int format) {
  switch (format) {
    case HAL_PIXEL_FORMAT_BGRA_8888: {
      auto* d = static_cast<uint32_t*>(dst);
      for (int x = 0; x < count; x++) {
        uint32_t p = src[x];
        d[x] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
      }
      break;
    }
    case HAL_PIXEL_FORMAT_RGB_565: {
      auto* d = static_cast<uint16_t*>(dst);
      for (int x = 0; x < count; x++) {
        uint32_t p = src[x];
        uint32_t r = p & 0xffu, g = (p >> 8) & 0xffu, b = (p >> 16) & 0xffu;
        d[x] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      }
      break;
    }
    case HAL_PIXEL_FORMAT_RGBX_8888: {
      auto* d = static_cast<uint32_t*>(dst);
      for (int x = 0; x < count; x++) d[x] = src[x] | 0xff000000u;
      break;
    }
    default:  // RGBA_8888
      std::memcpy(dst, src, static_cast<size_t>(count) * 4);
      break;
  }
}

int BytesPerPixel(int format) {
  return format == HAL_PIXEL_FORMAT_RGB_565 ? 2 : 4;
}

}  // namespace

HWND CreateChildHelper(ANativeWindow* window) {
  if (window == nullptr) return nullptr;
  if (GetAngle() == nullptr) return nullptr;
  auto* h = new Helper();
  WindowSize(window, &h->width, &h->height);
  auto* thread = new std::thread(HelperThread, h);
  HWND hwnd;
  {
    std::unique_lock<std::mutex> lock(h->mutex);
    h->cv.wait(lock, [h] { return h->done; });
    hwnd = h->hwnd;
  }
  if (hwnd == nullptr) {
    thread->join();
    delete thread;
    delete h;
    return nullptr;
  }
  delete h;  // the thread no longer touches it once the window exists
  std::lock_guard<std::mutex> lock(g_mutex);
  Threads()[hwnd] = thread;
  return hwnd;
}

void DestroyChildHelper(HWND helper) {
  std::thread* thread = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Threads().find(helper);
    if (it == Threads().end()) return;
    thread = it->second;
    Threads().erase(it);
  }
  DestroyHelperWindow(helper, thread);
}

void ChildSurfaceCreated(EGLSurface surface, ANativeWindow* window, HWND helper) {
  auto* c = new Child();
  c->window = window;
  c->helper = helper;
  WindowSize(window, &c->helper_width, &c->helper_height);
  std::fprintf(stderr,
               "gesher EGL: window surface on a %dx%d surface without a window of its own "
               "(a SurfaceView's or SurfaceTexture's): frames posted at each swap\n",
               c->helper_width, c->helper_height);
  std::fflush(stderr);
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = Threads().find(helper);
  if (it != Threads().end()) {
    c->thread = it->second;
    Threads().erase(it);
  }
  Children()[surface] = c;
}

ANativeWindow* ChildSurfaceWindow(EGLSurface surface) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = Children().find(surface);
  return it != Children().end() ? it->second->window : nullptr;
}

bool ChildSurfaceDestroyed(EGLSurface surface) {
  Child* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Children().find(surface);
    if (it == Children().end()) return false;
    c = it->second;
    Children().erase(it);
  }
  DestroyHelperWindow(c->helper, c->thread);
  delete c;
  return true;
}

void ChildSurfaceBeforeSwap(EGLDisplay display, EGLSurface surface) {
  Child* c = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Children().find(surface);
    if (it == Children().end()) return;
    c = it->second;
  }
  const Angle* gl = GetAngle();
  if (gl == nullptr || gl->get_current_surface(EGL_DRAW) != surface) return;
  EGLint w = 0, h = 0;
  gl->query_surface(display, surface, EGL_WIDTH, &w);
  gl->query_surface(display, surface, EGL_HEIGHT, &h);
  if (w > 0 && h > 0) {
    EGLint version = 0;
    gl->query_context(display, gl->get_current_context(), EGL_CONTEXT_CLIENT_VERSION, &version);
    // Read the default framebuffer, leaving the app's GL state as it was.
    GLint alignment = 4, framebuffer = 0, pack_buffer = 0, row_length = 0, skip_rows = 0,
          skip_pixels = 0;
    gl->get_integerv(GL_PACK_ALIGNMENT, &alignment);
    const GLenum fb_target = version >= 3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
    if (version >= 3) {
      gl->get_integerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
      gl->get_integerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
      gl->get_integerv(GL_PACK_ROW_LENGTH, &row_length);
      gl->get_integerv(GL_PACK_SKIP_ROWS, &skip_rows);
      gl->get_integerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    } else if (version == 2) {
      gl->get_integerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    }
    if (framebuffer != 0) gl->bind_framebuffer(fb_target, 0);
    if (version >= 3) {
      if (pack_buffer != 0) gl->bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
      if (row_length != 0) gl->pixel_storei(GL_PACK_ROW_LENGTH, 0);
      if (skip_rows != 0) gl->pixel_storei(GL_PACK_SKIP_ROWS, 0);
      if (skip_pixels != 0) gl->pixel_storei(GL_PACK_SKIP_PIXELS, 0);
    }
    if (alignment != 4) gl->pixel_storei(GL_PACK_ALIGNMENT, 4);
    c->pixels.resize(static_cast<size_t>(w) * h);
    gl->read_pixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels.data());
    if (alignment != 4) gl->pixel_storei(GL_PACK_ALIGNMENT, alignment);
    if (version >= 3) {
      if (skip_pixels != 0) gl->pixel_storei(GL_PACK_SKIP_PIXELS, skip_pixels);
      if (skip_rows != 0) gl->pixel_storei(GL_PACK_SKIP_ROWS, skip_rows);
      if (row_length != 0) gl->pixel_storei(GL_PACK_ROW_LENGTH, row_length);
      if (pack_buffer != 0) gl->bind_buffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
    }
    if (framebuffer != 0) gl->bind_framebuffer(fb_target, static_cast<GLuint>(framebuffer));

    // Post it: GL's rows are bottom-up.
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(c->window, &b, nullptr) == 0) {
      int rows = h < b.height ? h : b.height;
      int cols = w < b.width ? w : b.width;
      int bpp = BytesPerPixel(b.format);
      for (int y = 0; y < rows; y++) {
        PutRow(&c->pixels[static_cast<size_t>(h - 1 - y) * w],
               static_cast<uint8_t*>(b.bits) + static_cast<size_t>(y) * b.stride * bpp, cols,
               b.format);
      }
      ANativeWindow_unlockAndPost(c->window);
    }
  }
  // The window's size now (setFixedSize, a resized view): the helper follows,
  // and ANGLE resizes its swap chain at this swap.
  int ww, wh;
  WindowSize(c->window, &ww, &wh);
  if (ww != c->helper_width || wh != c->helper_height) {
    c->helper_width = ww;
    c->helper_height = wh;
    SetWindowPos(c->helper, nullptr, 0, 0, ww, wh,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
  // Paced as a 60 Hz display would (the hidden swap chain is not).
  auto now = std::chrono::steady_clock::now();
  auto next = c->last_post + std::chrono::microseconds(16667);
  if (now < next) std::this_thread::sleep_for(next - now);
  c->last_post = std::chrono::steady_clock::now();
}

}  // namespace gesher::bionic::gl
