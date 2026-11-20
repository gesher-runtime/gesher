// gesher (surfaces track, ADR 0056): EGL window surfaces on ANativeWindows
// without an HWND. See egl_child_surface.h.
//
// ANGLE is called through gesher_egl_proc (its Microsoft-ABI entry points),
// as code in the executable does; this file never goes through the System V
// thunks apps use.

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1
#include "egl_child_surface.h"
#include "egl_readback_layout.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#ifndef GL_PACK_REVERSE_ROW_ORDER_ANGLE
#define GL_PACK_REVERSE_ROW_ORDER_ANGLE 0x93A4
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

#include <gesher/egl_angle.h>
#include <gesher/perf_trace.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
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
  const GLubyte*(GL_APIENTRY* get_string)(GLenum);
  const GLubyte*(GL_APIENTRY* get_stringi)(GLenum, GLuint);
  void(GL_APIENTRY* pixel_storei)(GLenum, GLint);
  void(GL_APIENTRY* bind_framebuffer)(GLenum, GLuint);
  void(GL_APIENTRY* bind_buffer)(GLenum, GLuint);
  void(GL_APIENTRY* read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
  EGLBoolean(EGLAPIENTRY* swap_interval)(EGLDisplay, EGLint);
};

const Angle* GetAngle() {
  static const Angle* angle = [] {
    auto* a = new Angle();
    a->get_current_surface = Proc<decltype(a->get_current_surface)>("eglGetCurrentSurface");
    a->get_current_context = Proc<decltype(a->get_current_context)>("eglGetCurrentContext");
    a->query_surface = Proc<decltype(a->query_surface)>("eglQuerySurface");
    a->query_context = Proc<decltype(a->query_context)>("eglQueryContext");
    a->get_integerv = Proc<decltype(a->get_integerv)>("glGetIntegerv");
    a->get_string = Proc<decltype(a->get_string)>("glGetString");
    a->get_stringi = Proc<decltype(a->get_stringi)>("glGetStringi");  // optional on GLES2
    a->pixel_storei = Proc<decltype(a->pixel_storei)>("glPixelStorei");
    a->bind_framebuffer = Proc<decltype(a->bind_framebuffer)>("glBindFramebuffer");
    a->bind_buffer = Proc<decltype(a->bind_buffer)>("glBindBuffer");
    a->read_pixels = Proc<decltype(a->read_pixels)>("glReadPixels");
    a->swap_interval = Proc<decltype(a->swap_interval)>("eglSwapInterval");
    if (a->get_current_surface == nullptr || a->get_current_context == nullptr ||
        a->query_surface == nullptr || a->query_context == nullptr || a->get_integerv == nullptr ||
        a->get_string == nullptr || a->pixel_storei == nullptr || a->bind_framebuffer == nullptr ||
        a->bind_buffer == nullptr || a->read_pixels == nullptr || a->swap_interval == nullptr) {
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
  EGLContext last_context = EGL_NO_CONTEXT;
  bool interval_zero = false;  // ANGLE's swap interval set to 0 for it
  bool reverse_rows = false;
  bool bgra_read = false;
  bool es2_pack_buffer = false;
  bool es2_pack_subimage = false;
  bool force_copy = false;
  std::vector<uint32_t> pixels;  // scratch retained for conversion fallbacks
  ~Child();
};

std::mutex g_mutex;
std::unordered_map<HWND, std::thread*>& Threads() {
  static auto* map = new std::unordered_map<HWND, std::thread*>();
  return *map;
}
std::unordered_map<EGLSurface, std::shared_ptr<Child>>& Children() {
  static auto* map = new std::unordered_map<EGLSurface, std::shared_ptr<Child>>();
  return *map;
}

// Waits until `t` on a high-resolution waitable timer. sleep_for waits whole
// scheduler ticks (15.6 ms unless something raised the timer resolution), so
// a swap paced to 16.7 ms took up to 32: frames came out at irregular
// intervals, about 30 a second, and a video's frames were shown unevenly.
void WaitUntil(std::chrono::steady_clock::time_point t) {
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    ~Timer() {
      if (handle != nullptr) CloseHandle(handle);
    }
  };
  thread_local Timer timer;
  const auto now = std::chrono::steady_clock::now();
  if (t <= now) return;
  if (timer.handle != nullptr) {
    LARGE_INTEGER due;
    // Relative, in 100 ns units.
    due.QuadPart = -std::max<LONGLONG>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(t - now).count() / 100);
    if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0) {
      return;
    }
  }
  std::this_thread::sleep_until(t);
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

Child::~Child() {
  // An in-flight swap retains this object even if DestroySurface retires it.
  DestroyHelperWindow(helper, thread);
  if (window != nullptr) ANativeWindow_release(window);
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
  auto c = std::make_shared<Child>();
  c->window = window;
  // The EGL wrapper owns its original reference; this one survives retirement
  // while an in-flight swap is still reading the target.
  ANativeWindow_acquire(window);
  c->helper = helper;
  const char* direct = std::getenv("GESHER_EGL_DIRECT_READBACK");
  c->force_copy = direct != nullptr && std::strcmp(direct, "0") == 0;
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
  std::shared_ptr<Child> c;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Children().find(surface);
    if (it == Children().end()) return false;
    c = std::move(it->second);
    Children().erase(it);
  }
  // Release of the last frame reader closes the helper and its window ref.
  return true;
}

bool ChildSurfaceBeforeSwap(EGLDisplay display, EGLSurface surface) {
  std::shared_ptr<Child> c;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Children().find(surface);
    if (it == Children().end()) return true;
    c = it->second;
  }
  const Angle* gl = GetAngle();
  if (gl == nullptr || gl->get_current_surface(EGL_DRAW) != surface) return true;
  // No vertical blank to wait for (ChildSurfaceSwapInterval); ANGLE applies it
  // from this swap on.
  if (!c->interval_zero) {
    gl->swap_interval(display, 0);
    c->interval_zero = true;
  }

  // The window's size now (setFixedSize, a resized view): the helper follows,
  // and ANGLE resizes its swap chain at this swap.
  int ww, wh;
  WindowSize(c->window, &ww, &wh);
  bool resized = false;
  if (ww != c->helper_width || wh != c->helper_height) {
    c->helper_width = ww;
    c->helper_height = wh;
    SetWindowPos(c->helper, nullptr, 0, 0, ww, wh,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    resized = true;
  }

  EGLint w = 0, h = 0;
  gl->query_surface(display, surface, EGL_WIDTH, &w);
  gl->query_surface(display, surface, EGL_HEIGHT, &h);
  static gesher::perf::Site post_site("EGL child surface: read back and post a frame");
  const int64_t post_start = gesher::perf::Now();
  if (w > 0 && h > 0) {
    // Check the consumer before issuing a synchronous GPU readback.
    ANativeWindow_Buffer b = {};
    if (ANativeWindow_lock(c->window, &b, nullptr) == 0) {
      const EGLContext context = gl->get_current_context();
      EGLint version = 0;
      gl->query_context(display, context, EGL_CONTEXT_CLIENT_VERSION, &version);
      if (context != c->last_context) {
        c->last_context = context;
        // In GLES3, query each extension via glGetStringi; do not call the
        // GLES2-only legacy extension-string path in an ES3 context.
        const ReadbackExtensions extensions = QueryReadbackExtensions(
            version,
            [&]() -> const char* {
              return reinterpret_cast<const char*>(gl->get_string(GL_EXTENSIONS));
            },
            [&]() -> int {
              if (gl->get_stringi == nullptr) return 0;
              GLint count = 0;
              gl->get_integerv(GL_NUM_EXTENSIONS, &count);
              return count;
            },
            [&](int i) -> const char* {
              return reinterpret_cast<const char*>(
                  gl->get_stringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            });
        c->reverse_rows = extensions.reverse_rows;
        c->bgra_read = extensions.bgra_read;
        // GLES2 may expose PBOs only through the optional extension.
        c->es2_pack_buffer = extensions.es2_pack_buffer;
        c->es2_pack_subimage = extensions.es2_pack_subimage;
      }
      ReadbackLayout plan = PlanChildReadback(
          w, h, b.width, b.height, b.stride, b.format,
          c->reverse_rows && !c->force_copy, c->bgra_read, version >= 3);
      if (plan.valid() && b.bits != nullptr) {
        // Preserve all client pack state including reversed rows and PBOs.
        GLint alignment = 4, framebuffer = 0, pack_buffer = 0, row_length = 0, skip_rows = 0,
              skip_pixels = 0, reverse_order = 0;
        gl->get_integerv(GL_PACK_ALIGNMENT, &alignment);
        const GLenum fb_target = version >= 3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
        const bool pbo_available = version >= 3 || c->es2_pack_buffer;
        const bool pack_layout_available = version >= 3 || c->es2_pack_subimage;
        if (version >= 3) {
          gl->get_integerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
        } else if (version == 2) {
          gl->get_integerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        }
        // ANGLE exposes this layout state to GLES2 via NV_pack_subimage.
        // It affects both direct reads and the tightly packed staging buffer.
        if (pack_layout_available) {
          gl->get_integerv(GL_PACK_ROW_LENGTH, &row_length);
          gl->get_integerv(GL_PACK_SKIP_ROWS, &skip_rows);
          gl->get_integerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        }
        if (pbo_available) gl->get_integerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        if (c->reverse_rows) gl->get_integerv(GL_PACK_REVERSE_ROW_ORDER_ANGLE, &reverse_order);
        const bool direct = plan.direct != DirectReadFormat::kNone;
        const GLint desired_reverse = direct ? GL_TRUE : GL_FALSE;
        const GLint desired_row_length = direct ? b.stride : 0;
        if (framebuffer != 0) gl->bind_framebuffer(fb_target, 0);
        if (pbo_available && pack_buffer != 0) gl->bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
        if (pack_layout_available) {
          if (row_length != desired_row_length)
            gl->pixel_storei(GL_PACK_ROW_LENGTH, desired_row_length);
          if (skip_rows != 0) gl->pixel_storei(GL_PACK_SKIP_ROWS, 0);
          if (skip_pixels != 0) gl->pixel_storei(GL_PACK_SKIP_PIXELS, 0);
        }
        if (alignment != 4) gl->pixel_storei(GL_PACK_ALIGNMENT, 4);
        if (c->reverse_rows && reverse_order != desired_reverse)
          gl->pixel_storei(GL_PACK_REVERSE_ROW_ORDER_ANGLE, desired_reverse);
        static gesher::perf::Site read_site("EGL child surface: glReadPixels of a frame");
        const int64_t read_start = gesher::perf::Now();
        if (direct) {
          const GLenum format = plan.direct == DirectReadFormat::kBgra ? GL_BGRA_EXT : GL_RGBA;
          gl->read_pixels(0, plan.source_y, plan.width, plan.height,
                          format, GL_UNSIGNED_BYTE, b.bits);
        } else {
          c->pixels.resize(plan.pixels);
          gl->read_pixels(0, plan.source_y, plan.width, plan.height, GL_RGBA,
                          GL_UNSIGNED_BYTE, c->pixels.data());
        }
        read_site.Add(gesher::perf::Now() - read_start);
        if (c->reverse_rows && reverse_order != desired_reverse)
          gl->pixel_storei(GL_PACK_REVERSE_ROW_ORDER_ANGLE, reverse_order);
        if (alignment != 4) gl->pixel_storei(GL_PACK_ALIGNMENT, alignment);
        if (pack_layout_available) {
          if (skip_pixels != 0) gl->pixel_storei(GL_PACK_SKIP_PIXELS, skip_pixels);
          if (skip_rows != 0) gl->pixel_storei(GL_PACK_SKIP_ROWS, skip_rows);
          if (row_length != desired_row_length) gl->pixel_storei(GL_PACK_ROW_LENGTH, row_length);
        }
        if (pbo_available && pack_buffer != 0)
          gl->bind_buffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
        if (framebuffer != 0) gl->bind_framebuffer(fb_target, static_cast<GLuint>(framebuffer));
        if (!direct) CopyChildReadback(c->pixels.data(), b.bits, plan);
      }
      static gesher::perf::Site unlock_site("EGL child surface: unlockAndPost a frame");
      const int64_t unlock_start = gesher::perf::Now();
      ANativeWindow_unlockAndPost(c->window);
      unlock_site.Add(gesher::perf::Now() - unlock_start);
    }
  }
  post_site.Add(gesher::perf::Now() - post_start);
  // Paced as a 60 Hz display would (the hidden swap chain is not).
  static gesher::perf::Site pace_site("EGL child surface: 60 Hz pacing wait (count: ms late)");
  auto now = std::chrono::steady_clock::now();
  auto next = c->last_post + std::chrono::microseconds(16667);
  const int64_t pace_start = gesher::perf::Now();
  if (now < next) WaitUntil(next);
  c->last_post = std::chrono::steady_clock::now();
  if (now < next) {
    const auto late = std::chrono::duration_cast<std::chrono::milliseconds>(c->last_post - next);
    pace_site.Add(gesher::perf::Now() - pace_start, late.count() > 0 ? late.count() : 0);
  }
  // The frame is posted. Presenting the helper's swap chain shows nothing (the
  // helper is never shown) and held up the GPU: a later glReadPixels waited up
  // to half a second for Windows to take the presented buffer. ANGLE's swap is
  // still needed when the size changed: that is where it resizes the surface.
  return resized;
}

bool ChildSurfaceSwapInterval(EGLDisplay display, EGLBoolean* result) {
  const Angle* gl = GetAngle();
  if (gl == nullptr) return false;
  const EGLSurface surface = gl->get_current_surface(EGL_DRAW);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Children().find(surface);
    if (it == Children().end()) return false;
    it->second->interval_zero = true;
  }
  // ANGLE checks the display and the current context and surface as for any
  // interval (an interval is clamped, never an error); 0 leaves its swap chain
  // as it is.
  *result = gl->swap_interval(display, 0);
  return true;
}

}  // namespace gesher::bionic::gl
