// gesher: EGL and OpenGL ES for an app's ELF libraries (docs/native-libs.md,
// "Graphics"; ADR 0023). An app's .so imports eglXxx/glXxx from libEGL.so,
// libGLESv1_CM.so, libGLESv2.so or libGLESv3.so; the ELF loader satisfies
// those DT_NEEDED entries symbol by symbol from the bionic shim's table
// (like libc.so), and this file puts System V entries for every EGL and GLES
// function of the Khronos headers there. Each forwards to ANGLE
// (libEGL.dll/libGLESv2.dll next to the executable, Microsoft x64 ABI),
// loaded on first use; the thunks are generated from the headers
// (gen_gl_thunks.py -> gl_thunks.inc).
//
// Handwritten, where Android's EGL and ANGLE's differ:
//  - eglGetDisplay(EGL_DEFAULT_DISPLAY), and eglGetPlatformDisplay(EXT) for
//    EGL_PLATFORM_ANDROID_KHR: ANGLE's Direct3D 11 display on the hardware
//    adapter, initialized here to see that it works, else on WARP (the
//    environment variable GESHER_EGL_DEVICE=warp asks for WARP directly);
//  - eglCreateWindowSurface / eglCreatePlatformWindowSurface(EXT): the
//    native window is an ANativeWindow*; a Gesher window's (Win32Surface's)
//    HWND gets the ANGLE window surface. The surface holds a reference to the
//    ANativeWindow until eglDestroySurface, and while it exists the window's
//    own WM_PAINT (which paints the CPU back buffer, gesher_window.cc) is
//    switched off by subclassing the HWND, so GDI does not paint over what
//    the swap chain presents;
//  - eglChooseConfig drops the Android-only EGL_RECORDABLE_ANDROID and
//    EGL_FRAMEBUFFER_TARGET_ANDROID attributes (ANGLE rejects them);
//    eglGetConfigAttrib answers them, and EGL_NATIVE_VISUAL_ID as an Android
//    HAL pixel format (what NDK code passes to ANativeWindow_setBuffersGeometry)
//    instead of ANGLE's DXGI format;
//  - eglGetProcAddress returns the System V thunk of the name;
//  - callbacks the .so passes (glDebugMessageCallback(KHR),
//    eglDebugMessageControlKHR, eglSetBlobCacheFuncsANDROID) are System V
//    code: ANGLE gets Microsoft-ABI trampolines that call them.

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1  // EGLNative*Type are void*, as on Android
// The extension prototypes, for decltype(&::eglXxx) (MS() below).
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#include "abi_types.h"

#include <gesher/bionic_shim.h>
#include <gesher/egl_angle.h>
#include <gesher/elf_loader.h>

#include <android/native_window.h>

#include "gesher_native_window.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x320A
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE 0x320B
#endif

namespace gesher::bionic::gl {

enum Lib : uint8_t { kLibEgl, kLibGles };
struct GlName {
  const char* name;
  Lib lib;
};
struct GlThunk {
  const char* name;
  void* thunk;
};

void* ResolveGl(int index);
extern std::atomic<void*> g_gl_fn[];

// ANGLE's entry point for function `index` (gl_thunks.inc's I_<name>).
inline void* Fn(int index) {
  void* p = g_gl_fn[index].load(std::memory_order_acquire);
  return p != nullptr ? p : ResolveGl(index);
}

}  // namespace gesher::bionic::gl

#include "gl_thunks.inc"

namespace gesher::bionic::gl {

std::atomic<void*> g_gl_fn[kGlCount];

namespace {

// --- ANGLE ---------------------------------------------------------------------------------------

struct Angle {
  HMODULE egl = nullptr;
  HMODULE gles = nullptr;
};

HMODULE LoadNextToExe(const wchar_t* name) {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      p.resize(slash + 1);
      p += name;
      if (HMODULE m = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return m;
    }
  }
  return LoadLibraryW(name);
}

const Angle& GetAngle() {
  static const Angle angle = [] {
    Angle a;
    // libGLESv2.dll first: libEGL.dll is a thin layer over it.
    a.gles = LoadNextToExe(L"libGLESv2.dll");
    a.egl = LoadNextToExe(L"libEGL.dll");
    if (a.egl == nullptr || a.gles == nullptr) {
      std::fprintf(stderr,
                   "gesher EGL/GLES: cannot load ANGLE (libEGL.dll, libGLESv2.dll next to the "
                   "executable): error %lu\n",
                   GetLastError());
      std::fflush(stderr);
    }
    return a;
  }();
  return angle;
}

bool AngleAvailable() {
  const Angle& a = GetAngle();
  return a.egl != nullptr && a.gles != nullptr;
}

}  // namespace

void* ResolveGl(int index) {
  const GlName& n = kGlNames[index];
  const Angle& a = GetAngle();
  HMODULE module = n.lib == kLibEgl ? a.egl : a.gles;
  void* p = module != nullptr ? reinterpret_cast<void*>(GetProcAddress(module, n.name)) : nullptr;
  if (p == nullptr && a.egl != nullptr && index != I_eglGetProcAddress) {
    // An extension libGLESv2.dll does not export by name.
    auto get_proc = reinterpret_cast<__eglMustCastToProperFunctionPointerType(EGLAPIENTRY*)(
        const char*)>(Fn(I_eglGetProcAddress));
    p = reinterpret_cast<void*>(get_proc(n.name));
  }
  if (p == nullptr) {
    std::fprintf(stderr, "gesher EGL/GLES: %s is not provided by ANGLE (%s)\n", n.name,
                 module != nullptr ? "not exported" : "ANGLE is not loaded");
    std::fflush(stderr);
    std::abort();
  }
  g_gl_fn[index].store(p, std::memory_order_release);
  return p;
}

namespace {

// ANGLE's function `index`, typed as F (a Microsoft-ABI function pointer).
template <typename F>
F Ms(int index) {
  return reinterpret_cast<F>(Fn(index));
}

#define MS(name) Ms<decltype(&::name)>(I_##name)

void Log(const char* text) {
  std::fprintf(stderr, "gesher EGL: %s\n", text);
  std::fflush(stderr);
}

// --- displays ------------------------------------------------------------------------------------

EGLDisplay OpenAngle(EGLint device_type) {
  const EGLint attribs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                            EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, device_type, EGL_NONE};
  EGLDisplay d = MS(eglGetPlatformDisplayEXT)(EGL_PLATFORM_ANGLE_ANGLE,
                                              reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY), attribs);
  if (d == EGL_NO_DISPLAY) return EGL_NO_DISPLAY;
  EGLint major = 0, minor = 0;
  if (MS(eglInitialize)(d, &major, &minor) != EGL_TRUE) return EGL_NO_DISPLAY;
  return d;
}

// Android's default display: ANGLE on Direct3D 11, the hardware adapter if it
// initializes, else WARP. Initialized here (an app's own eglInitialize then
// only reports the version).
EGLDisplay DefaultDisplay() {
  static const EGLDisplay display = [] {
    if (!AngleAvailable()) return EGL_NO_DISPLAY;
    const char* want = std::getenv("GESHER_EGL_DEVICE");
    bool warp_only = want != nullptr && std::strcmp(want, "warp") == 0;
    if (!warp_only) {
      EGLDisplay d = OpenAngle(EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE);
      if (d != EGL_NO_DISPLAY) {
        Log("default display: ANGLE, Direct3D 11 hardware device");
        return d;
      }
      Log("Direct3D 11 hardware device failed; trying WARP");
    }
    EGLDisplay d = OpenAngle(EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE);
    Log(d != EGL_NO_DISPLAY ? "default display: ANGLE, Direct3D 11 WARP device"
                            : "no Direct3D 11 device (hardware or WARP): EGL_NO_DISPLAY");
    return d;
  }();
  return display;
}

EGLDisplay BIONIC_SYSV GetDisplay(void* /*native_display*/) {
  // Android has one display; whatever the app passes, it is the default one.
  return DefaultDisplay();
}

EGLDisplay BIONIC_SYSV GetPlatformDisplay(EGLenum platform, void* native_display,
                                          const EGLAttrib* attribs) {
  if (platform == EGL_PLATFORM_ANDROID_KHR) return DefaultDisplay();
  if (!AngleAvailable()) return EGL_NO_DISPLAY;
  return MS(eglGetPlatformDisplay)(platform, native_display, attribs);
}

EGLDisplay BIONIC_SYSV GetPlatformDisplayExt(EGLenum platform, void* native_display,
                                             const EGLint* attribs) {
  if (platform == EGL_PLATFORM_ANDROID_KHR) return DefaultDisplay();
  if (!AngleAvailable()) return EGL_NO_DISPLAY;
  return MS(eglGetPlatformDisplayEXT)(platform, native_display, attribs);
}

// --- configs -------------------------------------------------------------------------------------

bool AndroidOnlyConfigAttrib(EGLint attribute) {
  return attribute == EGL_RECORDABLE_ANDROID || attribute == EGL_FRAMEBUFFER_TARGET_ANDROID;
}

EGLBoolean BIONIC_SYSV ChooseConfig(EGLDisplay dpy, const EGLint* attribs, EGLConfig* configs,
                                    EGLint config_size, EGLint* num_config) {
  std::vector<EGLint> list;
  if (attribs != nullptr) {
    for (const EGLint* a = attribs; *a != EGL_NONE; a += 2) {
      if (AndroidOnlyConfigAttrib(a[0])) continue;
      list.push_back(a[0]);
      list.push_back(a[1]);
    }
    list.push_back(EGL_NONE);
  }
  return MS(eglChooseConfig)(dpy, attribs != nullptr ? list.data() : nullptr, configs,
                             config_size, num_config);
}

// Android's HAL_PIXEL_FORMAT_* values.
constexpr EGLint kHalRgba8888 = 1;
constexpr EGLint kHalRgbx8888 = 2;
constexpr EGLint kHalRgb565 = 4;
constexpr EGLint kHalRgba1010102 = 0x2b;
constexpr EGLint kHalRgbaFp16 = 0x16;

EGLBoolean BIONIC_SYSV GetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute,
                                       EGLint* value) {
  auto get = MS(eglGetConfigAttrib);
  if (AndroidOnlyConfigAttrib(attribute)) {
    // Every ANGLE config can go to a window or an encoder surface.
    EGLint ignored = 0;
    if (get(dpy, config, EGL_CONFIG_ID, &ignored) != EGL_TRUE) return EGL_FALSE;
    *value = EGL_TRUE;
    return EGL_TRUE;
  }
  if (attribute == EGL_NATIVE_VISUAL_ID) {
    EGLint r = 0, g = 0, b = 0, a = 0;
    if (get(dpy, config, EGL_RED_SIZE, &r) != EGL_TRUE) return EGL_FALSE;
    get(dpy, config, EGL_GREEN_SIZE, &g);
    get(dpy, config, EGL_BLUE_SIZE, &b);
    get(dpy, config, EGL_ALPHA_SIZE, &a);
    if (r == 5 && g == 6 && b == 5) {
      *value = kHalRgb565;
    } else if (r == 10) {
      *value = kHalRgba1010102;
    } else if (r == 16) {
      *value = kHalRgbaFp16;
    } else {
      *value = a > 0 ? kHalRgba8888 : kHalRgbx8888;
    }
    return EGL_TRUE;
  }
  return get(dpy, config, attribute, value);
}

// --- window surfaces -----------------------------------------------------------------------------

// While an EGL window surface is on a Gesher window, the window does not
// paint its CPU back buffer (WM_PAINT would draw over the swap chain's
// frames): the HWND is subclassed, from this thread (allowed within the
// process), and restored when the last such surface goes.
struct Takeover {
  WNDPROC previous = nullptr;
  int surfaces = 0;
};
std::mutex g_paint_mutex;
std::unordered_map<HWND, Takeover>& Takeovers() {
  static auto* map = new std::unordered_map<HWND, Takeover>();
  return *map;
}

LRESULT CALLBACK GlWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  WNDPROC previous = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_paint_mutex);
    auto it = Takeovers().find(hwnd);
    if (it != Takeovers().end()) previous = it->second.previous;
  }
  switch (msg) {
    case WM_PAINT:
      ValidateRect(hwnd, nullptr);  // the swap chain presents
      return 0;
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return previous != nullptr ? CallWindowProcW(previous, hwnd, msg, wparam, lparam)
                             : DefWindowProcW(hwnd, msg, wparam, lparam);
}

void TakeOverPainting(HWND hwnd) {
  std::lock_guard<std::mutex> lock(g_paint_mutex);
  Takeover& t = Takeovers()[hwnd];
  if (t.surfaces++ == 0) {
    t.previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GlWindowProc)));
  }
}

void GiveBackPainting(HWND hwnd) {
  std::lock_guard<std::mutex> lock(g_paint_mutex);
  auto it = Takeovers().find(hwnd);
  if (it == Takeovers().end() || --it->second.surfaces > 0) return;
  if (IsWindow(hwnd) && GetWindowLongPtrW(hwnd, GWLP_WNDPROC) ==
                            reinterpret_cast<LONG_PTR>(&GlWindowProc)) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.previous));
    InvalidateRect(hwnd, nullptr, FALSE);  // the back buffer shows again
  }
  Takeovers().erase(it);
}

struct WindowSurface {
  ANativeWindow* window;
  HWND hwnd;
};
std::mutex g_surface_mutex;
std::unordered_map<EGLSurface, WindowSurface>& WindowSurfaces() {
  static auto* map = new std::unordered_map<EGLSurface, WindowSurface>();
  return *map;
}

std::atomic<bool> g_said_not_gesher{false};

EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                               const EGLint* attribs) {
  auto create = MS(eglCreateWindowSurface);
  auto* window = static_cast<ANativeWindow*>(native_window);
  HWND hwnd = window != nullptr ? static_cast<HWND>(gesher_window_hwnd(window)) : nullptr;
  if (hwnd == nullptr) {
    if (!g_said_not_gesher.exchange(true)) {
      Log("eglCreateWindowSurface: the ANativeWindow is not a Gesher window (no HWND)");
    }
    // ANGLE reports EGL_BAD_NATIVE_WINDOW for it.
    return create(dpy, config, nullptr, attribs);
  }
  ANativeWindow_acquire(window);
  EGLSurface surface = create(dpy, config, hwnd, attribs);
  if (surface == EGL_NO_SURFACE) {
    ANativeWindow_release(window);
    return surface;
  }
  {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    WindowSurfaces()[surface] = WindowSurface{window, hwnd};
  }
  TakeOverPainting(hwnd);
  return surface;
}

EGLSurface BIONIC_SYSV CreateWindowSurfaceSysv(EGLDisplay dpy, EGLConfig config,
                                               void* native_window, const EGLint* attribs) {
  return CreateWindowSurface(dpy, config, native_window, attribs);
}

std::vector<EGLint> ToEglInt(const EGLAttrib* attribs) {
  std::vector<EGLint> list;
  if (attribs != nullptr) {
    for (const EGLAttrib* a = attribs; *a != EGL_NONE; a += 2) {
      list.push_back(static_cast<EGLint>(a[0]));
      list.push_back(static_cast<EGLint>(a[1]));
    }
  }
  list.push_back(EGL_NONE);
  return list;
}

EGLSurface BIONIC_SYSV CreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config,
                                                   void* native_window, const EGLAttrib* attribs) {
  // For EGL_PLATFORM_ANDROID_KHR the native window is the ANativeWindow*.
  std::vector<EGLint> list = ToEglInt(attribs);
  return CreateWindowSurface(dpy, config, native_window, list.data());
}

EGLSurface BIONIC_SYSV CreatePlatformWindowSurfaceExt(EGLDisplay dpy, EGLConfig config,
                                                      void* native_window, const EGLint* attribs) {
  return CreateWindowSurface(dpy, config, native_window, attribs);
}

EGLBoolean BIONIC_SYSV DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  EGLBoolean ok = MS(eglDestroySurface)(dpy, surface);
  WindowSurface ws{nullptr, nullptr};
  {
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    auto it = WindowSurfaces().find(surface);
    if (it != WindowSurfaces().end()) {
      ws = it->second;
      WindowSurfaces().erase(it);
    }
  }
  if (ws.window != nullptr) {
    GiveBackPainting(ws.hwnd);
    ANativeWindow_release(ws.window);
  }
  return ok;
}

// --- callbacks -----------------------------------------------------------------------------------

using SysvDebugProc = void(BIONIC_SYSV*)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                         GLsizei length, const GLchar* message,
                                         const void* user_param);
struct DebugCallback {
  SysvDebugProc fn;
  const void* user_param;
};

void KHRONOS_APIENTRY DebugTrampoline(GLenum source, GLenum type, GLuint id, GLenum severity,
                                      GLsizei length, const GLchar* message,
                                      const void* user_param) {
  auto* cb = static_cast<const DebugCallback*>(user_param);
  gesher::elf::SetupThreadTls();
  cb->fn(source, type, id, severity, length, message, cb->user_param);
}

// The callback goes to ANGLE as a trampoline whose user parameter carries the
// .so's function and parameter (kept for the process: GL may still call it).
template <typename F>
void SetDebugCallback(F set, SysvDebugProc callback, const void* user_param) {
  if (callback == nullptr) {
    set(nullptr, user_param);
    return;
  }
  set(&DebugTrampoline, new DebugCallback{callback, user_param});
}

void BIONIC_SYSV DebugMessageCallback(SysvDebugProc callback, const void* user_param) {
  SetDebugCallback(MS(glDebugMessageCallback), callback, user_param);
}

void BIONIC_SYSV DebugMessageCallbackKhr(SysvDebugProc callback, const void* user_param) {
  SetDebugCallback(MS(glDebugMessageCallbackKHR), callback, user_param);
}

using SysvEglDebugProc = void(BIONIC_SYSV*)(EGLenum error, const char* command,
                                            EGLint message_type, EGLLabelKHR thread_label,
                                            EGLLabelKHR object_label, const char* message);
std::atomic<SysvEglDebugProc> g_egl_debug{nullptr};

void EGLAPIENTRY EglDebugTrampoline(EGLenum error, const char* command, EGLint message_type,
                                    EGLLabelKHR thread_label, EGLLabelKHR object_label,
                                    const char* message) {
  if (SysvEglDebugProc fn = g_egl_debug.load()) {
    gesher::elf::SetupThreadTls();
    fn(error, command, message_type, thread_label, object_label, message);
  }
}

EGLint BIONIC_SYSV DebugMessageControlKhr(SysvEglDebugProc callback, const EGLAttrib* attribs) {
  g_egl_debug.store(callback);
  return MS(eglDebugMessageControlKHR)(callback != nullptr ? &EglDebugTrampoline : nullptr,
                                       attribs);
}

using SysvSetBlob = void(BIONIC_SYSV*)(const void* key, EGLsizeiANDROID key_size,
                                       const void* value, EGLsizeiANDROID value_size);
using SysvGetBlob = EGLsizeiANDROID(BIONIC_SYSV*)(const void* key, EGLsizeiANDROID key_size,
                                                  void* value, EGLsizeiANDROID value_size);
std::atomic<SysvSetBlob> g_set_blob{nullptr};
std::atomic<SysvGetBlob> g_get_blob{nullptr};

void SetBlobTrampoline(const void* key, EGLsizeiANDROID key_size, const void* value,
                       EGLsizeiANDROID value_size) {
  gesher::elf::SetupThreadTls();
  g_set_blob.load()(key, key_size, value, value_size);
}

EGLsizeiANDROID GetBlobTrampoline(const void* key, EGLsizeiANDROID key_size, void* value,
                                  EGLsizeiANDROID value_size) {
  gesher::elf::SetupThreadTls();
  return g_get_blob.load()(key, key_size, value, value_size);
}

void BIONIC_SYSV SetBlobCacheFuncs(EGLDisplay dpy, SysvSetBlob set, SysvGetBlob get) {
  g_set_blob.store(set);
  g_get_blob.store(get);
  MS(eglSetBlobCacheFuncsANDROID)(dpy, set != nullptr ? &SetBlobTrampoline : nullptr,
                                  get != nullptr ? &GetBlobTrampoline : nullptr);
}

// --- the table -----------------------------------------------------------------------------------

const GlThunk kHandwritten[] = {
    {"eglGetDisplay", reinterpret_cast<void*>(&GetDisplay)},
    {"eglGetPlatformDisplay", reinterpret_cast<void*>(&GetPlatformDisplay)},
    {"eglGetPlatformDisplayEXT", reinterpret_cast<void*>(&GetPlatformDisplayExt)},
    {"eglChooseConfig", reinterpret_cast<void*>(&ChooseConfig)},
    {"eglGetConfigAttrib", reinterpret_cast<void*>(&GetConfigAttrib)},
    {"eglCreateWindowSurface", reinterpret_cast<void*>(&CreateWindowSurfaceSysv)},
    {"eglCreatePlatformWindowSurface", reinterpret_cast<void*>(&CreatePlatformWindowSurface)},
    {"eglCreatePlatformWindowSurfaceEXT", reinterpret_cast<void*>(&CreatePlatformWindowSurfaceExt)},
    {"eglDestroySurface", reinterpret_cast<void*>(&DestroySurface)},
    {"eglDebugMessageControlKHR", reinterpret_cast<void*>(&DebugMessageControlKhr)},
    {"eglSetBlobCacheFuncsANDROID", reinterpret_cast<void*>(&SetBlobCacheFuncs)},
    {"glDebugMessageCallback", reinterpret_cast<void*>(&DebugMessageCallback)},
    {"glDebugMessageCallbackKHR", reinterpret_cast<void*>(&DebugMessageCallbackKhr)},
    // eglGetProcAddress is added by Register().
};

std::unordered_map<std::string, void*>& ByName() {
  static auto* map = new std::unordered_map<std::string, void*>();
  return *map;
}

__eglMustCastToProperFunctionPointerType BIONIC_SYSV EglGetProcAddress(const char* name) {
  if (name == nullptr) return nullptr;
  auto it = ByName().find(name);
  return it != ByName().end() ? reinterpret_cast<__eglMustCastToProperFunctionPointerType>(it->second)
                              : nullptr;
}

void Register() {
  auto add = [](const char* name, void* fn) {
    ByName()[name] = fn;
    AddBionicSymbol(name, fn);
  };
  for (const GlThunk& t : kGlThunks) add(t.name, t.thunk);
  for (const GlThunk& t : kHandwritten) add(t.name, t.thunk);
  add("eglGetProcAddress", reinterpret_cast<void*>(&EglGetProcAddress));
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::bionic::gl

// --- for code in the executable (Microsoft ABI): gesher/egl_angle.h -------------------------------
// hwui's EglManager (compat/hwui/gl/angle_gl.cc, ADR 0030) takes the same
// displays, configs and window surfaces as an app's .so.

extern "C" void* gesher_egl_proc(const char* name) {
  namespace gl = gesher::bionic::gl;
  if (name == nullptr || !gl::AngleAvailable()) return nullptr;
  const gl::Angle& a = gl::GetAngle();
  if (FARPROC p = GetProcAddress(a.gles, name)) return reinterpret_cast<void*>(p);
  if (FARPROC p = GetProcAddress(a.egl, name)) return reinterpret_cast<void*>(p);
  using GetProc = __eglMustCastToProperFunctionPointerType(EGLAPIENTRY*)(const char*);
  auto get_proc = reinterpret_cast<GetProc>(
      reinterpret_cast<void*>(GetProcAddress(a.egl, "eglGetProcAddress")));
  return get_proc != nullptr ? reinterpret_cast<void*>(get_proc(name)) : nullptr;
}

extern "C" void* gesher_egl_default_display(void) {
  return gesher::bionic::gl::DefaultDisplay();
}

extern "C" unsigned gesher_egl_choose_config(void* display, const int32_t* attribs, void** configs,
                                             int32_t config_size, int32_t* num_config) {
  return gesher::bionic::gl::ChooseConfig(display, attribs, configs, config_size, num_config);
}

extern "C" unsigned gesher_egl_get_config_attrib(void* display, void* config, int32_t attribute,
                                                 int32_t* value) {
  return gesher::bionic::gl::GetConfigAttrib(display, config, attribute, value);
}

extern "C" void* gesher_egl_create_window_surface(void* display, void* config,
                                                  ANativeWindow* window, const int32_t* attribs) {
  return gesher::bionic::gl::CreateWindowSurface(display, config, window, attribs);
}

extern "C" unsigned gesher_egl_destroy_surface(void* display, void* surface) {
  return gesher::bionic::gl::DestroySurface(display, surface);
}

extern "C" void* gesher_egl_surface_hwnd(void* surface) {
  namespace gl = gesher::bionic::gl;
  std::lock_guard<std::mutex> lock(gl::g_surface_mutex);
  auto it = gl::WindowSurfaces().find(surface);
  return it != gl::WindowSurfaces().end() ? static_cast<void*>(it->second.hwnd) : nullptr;
}
