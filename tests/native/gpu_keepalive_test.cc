// gesher: the GPU keep-alive between frames read back (compat/bionic/gpu_keepalive.h)
// on the default EGL display (ANGLE's Direct3D 11 device). On a hardware
// adapter it gives the GPU work in the gaps of frames coming 25 a second, not
// while frames come faster than its 10 ms, and stops a second after the last
// frame until the next one. On a software adapter (WARP, the CI runners' Basic
// Render Driver) and with GESHER_GPU_KEEPALIVE=0 it stays off.
// Prints "gpu_keepalive_test all passed".
#include "gpu_keepalive.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <timeapi.h>

#include <gesher/egl_angle.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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

}  // namespace

int main() {
  timeBeginPeriod(1);
  void* display = gesher_egl_default_display();
  if (display == nullptr) {
    std::printf("gpu_keepalive_test: no EGL display\n");
    return 1;
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
