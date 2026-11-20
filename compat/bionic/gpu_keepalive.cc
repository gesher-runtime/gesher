// gesher: keeps the GPU from sitting idle between the frames Gesher reads back.
// See gpu_keepalive.h.

#include "gpu_keepalive.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <gesher/egl_angle.h>
#include <gesher/perf_trace.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace gesher::bionic {
namespace {

// No frame for this long: the thread gives the GPU a little work.
constexpr int64_t kIdleNs = 10'000'000;
// How often the thread looks.
constexpr int64_t kTickNs = 10'000'000;
// No frame for this long: the thread waits for the next frame.
constexpr int64_t kStopNs = 1'000'000'000;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class KeepAlive {
 public:
  KeepAlive(ID3D11DeviceContext* context, ID3D11RenderTargetView* target)
      : context_(context), target_(target) {}

  void Frame() {
    last_frame_.store(NowNs());
    if (sleeping_.load()) {
      std::lock_guard<std::mutex> lock(mutex_);
      cv_.notify_one();
    }
  }

  void Run() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    static gesher::perf::Site site("GPU keep-alive: a clear and flush between frames");
    const float clear[4] = {0, 0, 0, 0};
    for (;;) {
      const int64_t idle = NowNs() - last_frame_.load();
      if (idle >= kStopNs) {
        std::unique_lock<std::mutex> lock(mutex_);
        sleeping_.store(true);
        cv_.wait(lock, [this] { return NowNs() - last_frame_.load() < kStopNs; });
        sleeping_.store(false);
        continue;
      }
      if (idle >= kIdleNs) {
        const int64_t start = gesher::perf::Now();
        context_->ClearRenderTargetView(target_, clear);
        context_->Flush();
        site.Add(gesher::perf::Now() - start);
        submissions_.fetch_add(1);
      }
      LARGE_INTEGER due;
      due.QuadPart = -kTickNs / 100;  // relative, in 100 ns units
      if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
      } else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(kTickNs));
      }
    }
  }

  uint64_t submissions() const { return submissions_.load(); }

 private:
  // Kept for the process's life with the thread (never released), as is
  // their device.
  ID3D11DeviceContext* context_;
  ID3D11RenderTargetView* target_;
  std::atomic<int64_t> last_frame_{0};
  std::atomic<bool> sleeping_{false};
  std::atomic<uint64_t> submissions_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
};

bool Disabled() {
  const char* v = std::getenv("GESHER_GPU_KEEPALIVE");
  return v != nullptr && std::strcmp(v, "0") == 0;
}

// The keep-alive on the adapter of `frames` (its thread started); null without
// one (a software adapter, no device, turned off).
KeepAlive* Start(ID3D11Device* frames) {
  if (frames == nullptr || Disabled()) return nullptr;
  IDXGIDevice* dxgi = nullptr;
  IDXGIAdapter* adapter = nullptr;
  if (FAILED(frames->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) return nullptr;
  HRESULT hr = dxgi->GetAdapter(&adapter);
  dxgi->Release();
  if (FAILED(hr)) return nullptr;
  IDXGIAdapter1* adapter1 = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};
  bool software = false;
  if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter1)))) {
    if (SUCCEEDED(adapter1->GetDesc1(&desc))) software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    adapter1->Release();
  }
  if (software) {  // WARP: no GPU that sleeps
    adapter->Release();
    return nullptr;
  }
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                         nullptr, &context);
  adapter->Release();
  if (FAILED(hr)) {
    std::fprintf(stderr, "gesher GPU keep-alive: D3D11CreateDevice failed: 0x%08lx\n",
                 static_cast<unsigned long>(hr));
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 1;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* texture = nullptr;
  ID3D11RenderTargetView* target = nullptr;
  if (FAILED(device->CreateTexture2D(&td, nullptr, &texture)) ||
      FAILED(device->CreateRenderTargetView(texture, nullptr, &target))) {
    if (texture != nullptr) texture->Release();
    context->Release();
    device->Release();
    return nullptr;
  }
  texture->Release();  // the view holds it
  auto* k = new KeepAlive(context, target);
  std::thread([k] { k->Run(); }).detach();
  std::fprintf(stderr, "gesher GPU keep-alive: on, adapter %ls\n", desc.Description);
  return k;
}

// ANGLE's Direct3D 11 device of `display` (EGL_EXT_device_query); null otherwise.
ID3D11Device* AngleDevice(void* display) {
  using QueryDisplay = unsigned (*)(void*, int32_t, intptr_t*);
  using QueryDevice = unsigned (*)(void*, int32_t, intptr_t*);
  constexpr int32_t kEglDeviceExt = 0x322C;
  constexpr int32_t kEglD3D11DeviceAngle = 0x33A1;
  auto query_display = reinterpret_cast<QueryDisplay>(gesher_egl_proc("eglQueryDisplayAttribEXT"));
  auto query_device = reinterpret_cast<QueryDevice>(gesher_egl_proc("eglQueryDeviceAttribEXT"));
  intptr_t egl_device = 0, d3d = 0;
  if (display == nullptr || query_display == nullptr || query_device == nullptr ||
      !query_display(display, kEglDeviceExt, &egl_device) || egl_device == 0 ||
      !query_device(reinterpret_cast<void*>(egl_device), kEglD3D11DeviceAngle, &d3d)) {
    return nullptr;
  }
  return reinterpret_cast<ID3D11Device*>(d3d);
}

std::once_flag g_once;
std::atomic<KeepAlive*> g_keep_alive{nullptr};

}  // namespace

void GpuKeepAliveAfterFrame(void* egl_display) {
  std::call_once(g_once, [egl_display] { g_keep_alive.store(Start(AngleDevice(egl_display))); });
  if (KeepAlive* k = g_keep_alive.load()) k->Frame();
}

void GpuKeepAliveAfterFrameOn(ID3D11Device* device) {
  std::call_once(g_once, [device] { g_keep_alive.store(Start(device)); });
  if (KeepAlive* k = g_keep_alive.load()) k->Frame();
}

bool GpuKeepAliveStarted() { return g_keep_alive.load() != nullptr; }

uint64_t GpuKeepAliveSubmissions() {
  KeepAlive* k = g_keep_alive.load();
  return k != nullptr ? k->submissions() : 0;
}

}  // namespace gesher::bionic
