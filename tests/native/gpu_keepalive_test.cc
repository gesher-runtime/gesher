// gesher: the GPU keep-alive between frames read back (compat/bionic/gpu_keepalive.h)
// on the default EGL display (ANGLE's Direct3D 11 device). On a hardware
// adapter it gives the GPU work in the gaps of frames coming 25 a second, not
// while frames come faster than its 10 ms, and stops a second after the last
// frame until the next one. On a software adapter (WARP, the CI runners' Basic
// Render Driver) and with GESHER_GPU_KEEPALIVE=0 it stays off.
// It also checks the OpenGL ES version declared for that display
// (gesher_egl_gles_version, compat/bionic/egl_gles_version.h) against what EGL
// gives: 3.0 only on a hardware adapter; on a software one (WARP, the CI
// runners' Basic Render Driver) no config carries EGL_OPENGL_ES3_BIT_KHR, an
// ES 3 context is refused with EGL_BAD_MATCH and GL_VERSION is OpenGL ES 2.0.
// Prints "gpu_keepalive_test all passed".
#include "gpu_keepalive.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <timeapi.h>

#include <gesher/egl_angle.h>
#include "declared_boot.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using gesher::bionic::GpuKeepAliveAfterFrame;
using gesher::bionic::GpuKeepAliveStarted;
using gesher::bionic::GpuKeepAliveSubmissions;

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
  if (!ok) failures++;
}

// Whether ANGLE's device for `display` is on a software adapter.
bool SoftwareAdapter(void* display) {
  using Query = unsigned (*)(void*, int32_t, intptr_t*);
  auto query_display = reinterpret_cast<Query>(gesher_egl_proc("eglQueryDisplayAttribEXT"));
  auto query_device = reinterpret_cast<Query>(gesher_egl_proc("eglQueryDeviceAttribEXT"));
  intptr_t egl_device = 0, d3d = 0;
  if (query_display == nullptr || query_device == nullptr || !query_display(display, 0x322C, &egl_device) ||
      !query_device(reinterpret_cast<void*>(egl_device), 0x33A1, &d3d) || d3d == 0) {
    std::printf("no Direct3D 11 device behind the display\n");
    return true;
  }
  auto* device = reinterpret_cast<ID3D11Device*>(d3d);
  IDXGIDevice* dxgi = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIAdapter1* adapter1 = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};
  bool software = true;
  if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
      SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter1))) &&
      SUCCEEDED(adapter1->GetDesc1(&desc))) {
    software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    std::printf("adapter: %ls%s\n", desc.Description, software ? " (software)" : "");
  }
  if (adapter1 != nullptr) adapter1->Release();
  if (adapter != nullptr) adapter->Release();
  if (dxgi != nullptr) dxgi->Release();
  return software;
}

// Frames every `period` for `duration`; returns how often the GPU was given work.
uint64_t Frames(void* display, std::chrono::microseconds period, std::chrono::milliseconds duration) {
  const uint64_t before = GpuKeepAliveSubmissions();
  const auto end = std::chrono::steady_clock::now() + duration;
  auto next = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < end) {
    GpuKeepAliveAfterFrame(display);
    next += period;
    while (std::chrono::steady_clock::now() < next) {
      if (next - std::chrono::steady_clock::now() > std::chrono::milliseconds(2)) Sleep(1);
    }
  }
  return GpuKeepAliveSubmissions() - before;
}

// EGL and GL values (the test does not include the Khronos headers).
constexpr int32_t kEglNone = 0x3038, kEglRenderableType = 0x3040, kEglConformant = 0x3042,
                  kEglSurfaceType = 0x3033, kEglPbufferBit = 0x0001, kEglWidth = 0x3057,
                  kEglHeight = 0x3056, kEglContextClientVersion = 0x3098, kEglOpenglEs2Bit = 0x0004,
                  kEglOpenglEs3Bit = 0x0040, kEglBadMatch = 0x3009, kEglContextMinorVersion = 0x30FB;
constexpr uint32_t kGlVersion = 0x1F02;

// ANGLE's functions (Microsoft ABI, as this executable).
template <typename F>
F Angle(const char* name) {
  return reinterpret_cast<F>(gesher_egl_proc(name));
}

// Whether any of `display`'s configs is renderable or conformant as OpenGL ES 3.
bool AnyEs3Config(void* display) {
  using GetConfigs = unsigned (*)(void*, void**, int32_t, int32_t*);
  auto get_configs = Angle<GetConfigs>("eglGetConfigs");
  int32_t count = 0;
  if (get_configs == nullptr || !get_configs(display, nullptr, 0, &count) || count <= 0) return false;
  std::vector<void*> configs(static_cast<size_t>(count));
  if (!get_configs(display, configs.data(), count, &count)) return false;
  for (int32_t i = 0; i < count; ++i) {
    int32_t renderable = 0, conformant = 0;
    gesher_egl_get_config_attrib(display, configs[i], kEglRenderableType, &renderable);
    gesher_egl_get_config_attrib(display, configs[i], kEglConformant, &conformant);
    if (((renderable | conformant) & kEglOpenglEs3Bit) != 0) return true;
  }
  return false;
}

// A context of `version` on a pbuffer: its GL_VERSION, or "" with *error set
// to the EGL error when the context is refused.
std::string ContextVersion(void* display, int32_t version, int32_t* error, int32_t minor = 0) {
  using CreateContext = void* (*)(void*, void*, void*, const int32_t*);
  using CreatePbuffer = void* (*)(void*, void*, const int32_t*);
  using MakeCurrent = unsigned (*)(void*, void*, void*, void*);
  using Destroy = unsigned (*)(void*, void*);
  using GetError = int32_t (*)();
  using GetString = const char* (*)(uint32_t);
  // As an app's eglCreateContext: through Gesher's (no version above what the
  // default display gives).
  CreateContext create_context = [](void* d, void* c, void* s, const int32_t* a) {
    return gesher_egl_create_context(d, c, s, a);
  };
  auto create_pbuffer = Angle<CreatePbuffer>("eglCreatePbufferSurface");
  auto make_current = Angle<MakeCurrent>("eglMakeCurrent");
  auto destroy_context = Angle<Destroy>("eglDestroyContext");
  auto destroy_surface = Angle<Destroy>("eglDestroySurface");
  auto get_error = Angle<GetError>("eglGetError");
  auto get_string = Angle<GetString>("glGetString");
  *error = 0;
  // An ES 2 pbuffer config: one every display has, so a refused ES 3 context is
  // refused for its version (an ES 3 config is asked for when there is one).
  const int32_t config_attribs[] = {kEglRenderableType, version >= 3 && AnyEs3Config(display)
                                                            ? kEglOpenglEs3Bit : kEglOpenglEs2Bit,
                                    kEglSurfaceType, kEglPbufferBit, kEglNone};
  void* config = nullptr;
  int32_t count = 0;
  if (!gesher_egl_choose_config(display, config_attribs, &config, 1, &count) || count < 1) {
    *error = get_error();
    return "";
  }
  const int32_t context_attribs[] = {kEglContextClientVersion, version, kEglContextMinorVersion, minor, kEglNone};
  void* context = create_context(display, config, nullptr, context_attribs);
  if (context == nullptr) {
    *error = get_error();
    return "";
  }
  const int32_t surface_attribs[] = {kEglWidth, 4, kEglHeight, 4, kEglNone};
  void* surface = create_pbuffer(display, config, surface_attribs);
  std::string gl_version;
  if (surface != nullptr && make_current(display, surface, surface, context)) {
    const char* v = get_string(kGlVersion);
    gl_version = v != nullptr ? v : "(null)";
    make_current(display, nullptr, nullptr, nullptr);
  }
  if (surface != nullptr) destroy_surface(display, surface);
  destroy_context(display, context);
  return gl_version;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

int main(int argc, char** argv) {
  // --declared: this test as an app process of a boot that declared that version.
  int declared = 0;
  const int parent = gesher::test::RunAsDeclaredBoot(argc, argv, "gpu-keepalive-test", &declared);
  if (parent >= 0) return parent;
  timeBeginPeriod(1);
  // Asked first, as the system server does at boot: the display is chosen and
  // terminated again.
  const int probed = gesher_egl_gles_version();
  void* display = gesher_egl_default_display();
  if (display == nullptr) {
    std::printf("gpu_keepalive_test: no EGL display\n");
    return 1;
  }
  {
    const bool software_display = SoftwareAdapter(display);
    const bool es3 = AnyEs3Config(display);
    std::printf("declared OpenGL ES 0x%x (%s adapter, ES 3 configs %s)\n", probed,
                software_display ? "software" : "hardware", es3 ? "yes" : "no");
    Expect(gesher_egl_gles_version() == probed, "the declared version is the default display's");
    Expect(probed == (es3 ? 0x30000 : 0x20000), "the declared version is what the configs give");
    if (declared != 0) {
      // An app process follows the boot's declaration, whatever the adapter.
      Expect(probed == declared, declared == 0x30000
                                     ? "boot declared 3.0: this process's display gives ES 3.0"
                                     : "boot declared 2.0: this process's display gives ES 2.0 only");
    } else if (software_display) {
      Expect(!es3, "a software adapter: no EGL_OPENGL_ES3_BIT_KHR config");
      Expect(probed == 0x20000, "a software adapter declares OpenGL ES 2.0, not 3.0");
    }
    int32_t error = 0;
    const std::string v3 = ContextVersion(display, 3, &error);
    if (es3) {
      std::printf("ES 3 context: GL_VERSION %s\n", v3.c_str());
      Expect(StartsWith(v3, "OpenGL ES 3.0"), "ES 3 configs: an ES 3 context, GL_VERSION OpenGL ES 3.0");
      // Never more than declared: no ES 3.1 context (ANGLE's Direct3D 11 backend
      // could give one), refused as a 3.0 driver refuses it.
      const std::string v31 = ContextVersion(display, 3, &error, 1);
      std::printf("ES 3.1 context refused: EGL error 0x%x\n", static_cast<unsigned>(error));
      Expect(v31.empty() && error == kEglBadMatch, "an ES 3.0 display: an ES 3.1 context refused with EGL_BAD_MATCH");
    } else {
      std::printf("ES 3 context refused: EGL error 0x%x\n", static_cast<unsigned>(error));
      Expect(v3.empty() && error == kEglBadMatch, "no ES 3 configs: an ES 3 context refused with EGL_BAD_MATCH");
      const std::string v2 = ContextVersion(display, 2, &error);
      std::printf("ES 2 context: GL_VERSION %s\n", v2.c_str());
      Expect(StartsWith(v2, "OpenGL ES 2."), "no ES 3 configs: GL_VERSION OpenGL ES 2");
    }
  }
  const char* env = std::getenv("GESHER_GPU_KEEPALIVE");
  const bool turned_off = env != nullptr && std::strcmp(env, "0") == 0;
  const bool software = SoftwareAdapter(display);
  GpuKeepAliveAfterFrame(display);
  if (turned_off || software) {
    Expect(!GpuKeepAliveStarted(), turned_off ? "GESHER_GPU_KEEPALIVE=0: the keep-alive stays off"
                                              : "a software adapter: the keep-alive stays off");
    Expect(Frames(display, std::chrono::milliseconds(40), std::chrono::milliseconds(300)) == 0,
           "no work given to the GPU");
  } else {
    Expect(GpuKeepAliveStarted(), "a hardware adapter: the keep-alive runs");
    // 25 frames a second: each 40 ms gap gets work after 10 ms, every 10 ms.
    const uint64_t slow = Frames(display, std::chrono::milliseconds(40), std::chrono::milliseconds(1000));
    std::printf("frames 25 a second for 1 s: the GPU given work %llu times\n",
                static_cast<unsigned long long>(slow));
    Expect(slow >= 25, "frames 25 a second: work in every gap");
    // 500 frames a second: no gap reaches 10 ms.
    const uint64_t fast = Frames(display, std::chrono::microseconds(2000), std::chrono::milliseconds(500));
    std::printf("frames 500 a second for 0.5 s: the GPU given work %llu times\n",
                static_cast<unsigned long long>(fast));
    Expect(fast <= 3, "frames faster than every 10 ms: (almost) no work of its own");
    // A second after the last frame it stops.
    Sleep(1300);
    const uint64_t stopped = GpuKeepAliveSubmissions();
    Sleep(500);
    Expect(GpuKeepAliveSubmissions() == stopped, "a second after the last frame: no more work");
    // And starts again with the next frame.
    GpuKeepAliveAfterFrame(display);
    Sleep(100);
    Expect(GpuKeepAliveSubmissions() > stopped, "the next frame: work again in its gap");
  }
  if (failures != 0) {
    std::printf("gpu_keepalive_test: %d failed\n", failures);
    return 1;
  }
  std::printf("gpu_keepalive_test all passed\n");
  return 0;
}
