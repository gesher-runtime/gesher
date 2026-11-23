// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real ANGLE/D3D11 tests, including WARP. No fake GL completion or pipe fence.
// A second D3D11 device deliberately delays a genuine fence, making pending-state,
// server-wait and global-lock assertions deterministic without measuring FPS.
#include "gpu_interop.h"
#include "gpu_fence.h"
#include "egl_native_buffer.h"
#include "abi_types.h"
#include "native_fence_internal.h"
#include "internal.h"
#include <gesher/egl_angle.h>
#include <gesher/native_fence.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace {
std::atomic<int> failures{0};
void Check(bool success, const char* message) {
  std::printf("%s: %s\n", success ? "ok" : "FAILED", message);
  if (!success) failures.fetch_add(1);
}
std::unordered_map<std::string, void*> entries;
void Add(const char* name, void* entry) { entries[name] = entry; }
using Create = EGLSyncKHR(BIONIC_SYSV*)(EGLDisplay, EGLenum, const EGLint*);
using Destroy = EGLBoolean(BIONIC_SYSV*)(EGLDisplay, EGLSyncKHR);
using Wait = EGLint(BIONIC_SYSV*)(EGLDisplay, EGLSyncKHR, EGLint);
using ClientWait = EGLint(BIONIC_SYSV*)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
using Attrib = EGLBoolean(BIONIC_SYSV*)(EGLDisplay, EGLSyncKHR, EGLint, EGLint*);
using Duplicate = EGLint(BIONIC_SYSV*)(EGLDisplay, EGLSyncKHR);
Create create_sync;
Destroy destroy_sync;
Wait wait_sync;
ClientWait client_wait;
Attrib attrib;
Duplicate duplicate;
template <class F> F Gl(const char* name) { return reinterpret_cast<F>(gesher_egl_proc(name)); }

struct Context {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  bool Init(EGLDisplay dpy) {
    display = dpy;
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!Gl<decltype(&eglChooseConfig)>("eglChooseConfig")(display, config_attributes, &config, 1, &count) || !count) return false;
    const EGLint size[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    surface = Gl<decltype(&eglCreatePbufferSurface)>("eglCreatePbufferSurface")(display, config, size);
    context = Gl<decltype(&eglCreateContext)>("eglCreateContext")(display, config, EGL_NO_CONTEXT, version);
    return surface && context && Current();
  }
  bool Current() { return Gl<decltype(&eglMakeCurrent)>("eglMakeCurrent")(display, surface, surface, context) == EGL_TRUE; }
  ~Context() {
    Gl<decltype(&eglMakeCurrent)>("eglMakeCurrent")(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context) Gl<decltype(&eglDestroyContext)>("eglDestroyContext")(display, context);
    if (surface) Gl<decltype(&eglDestroySurface)>("eglDestroySurface")(display, surface);
  }
};

void Clear(float red, float green) {
  Gl<decltype(&glClearColor)>("glClearColor")(red, green, 0, 1);
  Gl<decltype(&glClear)>("glClear")(GL_COLOR_BUFFER_BIT);
}
bool Pixel(unsigned red, unsigned green) {
  unsigned char pixel[4]{};
  Gl<decltype(&glReadPixels)>("glReadPixels")(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  return pixel[0] == red && pixel[1] == green && pixel[2] == 0;
}

using Operation = void(__stdcall*)(ID3D11Device*, ID3D11DeviceContext*, void*);
using Dispatch = int(__stdcall*)(Operation, void*);

struct Gate {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext4> producer;
  ComPtr<ID3D11Fence> fence;
  HANDLE shared = nullptr;
  ~Gate() { if (shared) CloseHandle(shared); }
  bool Init(EGLDisplay display) {
    auto* gl = gesher::bionic::gpu::Functions();
    EGLAttrib egl_device = 0, angle = 0;
    if (!gl->eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &egl_device) ||
        !gl->eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(egl_device), EGL_D3D11_DEVICE_ANGLE, &angle)) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> device5;
    if (FAILED(reinterpret_cast<ID3D11Device*>(angle)->QueryInterface(__uuidof(IDXGIDevice),
          reinterpret_cast<void**>(dxgi.GetAddressOf()))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
          D3D11_SDK_VERSION, &device, nullptr, &context)) || FAILED(device.As(&device5)) || FAILED(context.As(&producer))) return false;
    HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
        reinterpret_cast<void**>(fence.GetAddressOf()));
    return SUCCEEDED(hr) && SUCCEEDED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
  }
  void Signal() { Check(SUCCEEDED(producer->Signal(fence.Get(), 1)), "producer signals the real GPU gate"); producer->Flush(); }
  bool QueueWait() {
    struct Request { Gate* gate; HRESULT result; } request{this, E_FAIL};
    auto callback = +[](ID3D11Device* device, ID3D11DeviceContext* context, void* data) __stdcall {
      auto* request = static_cast<Request*>(data);
      // Test-only GPU gate: deliberately stop the device queue until the
      // independent producer signals it. Production EGL never inserts a
      // foreign dependency onto ANGLE's shared immediate queue.
      ComPtr<ID3D11Device5> device5;
      ComPtr<ID3D11DeviceContext4> context4;
      ComPtr<ID3D11Fence> opened;
      if (FAILED(device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(device5.GetAddressOf()))) ||
          FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(context4.GetAddressOf())))) return;
      request->result = device5->OpenSharedFence(request->gate->shared, __uuidof(ID3D11Fence),
                                                reinterpret_cast<void**>(opened.GetAddressOf()));
      if (SUCCEEDED(request->result)) request->result = context4->Wait(opened.Get(), 1);
    };
    auto dispatch = Gl<Dispatch>("gesherAngleD3D11ContextV1");
    return dispatch && dispatch(callback, &request) && SUCCEEDED(request.result);
  }
};

// A watchdog releases the GPU if an old synchronous path regresses, producing
// a failing assertion instead of hanging the machine or the CI worker.
struct Release {
  HANDLE request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<bool> watchdog{false};
  std::thread thread;
  explicit Release(Gate& gate) : thread([this, &gate] {
    if (WaitForSingleObject(request, 1500) != WAIT_OBJECT_0) watchdog.store(true);
    gate.Signal();
  }) {}
  void Now() { SetEvent(request); if (thread.joinable()) thread.join(); }
  ~Release() { Now(); CloseHandle(request); }
};

void PendingEgl(EGLDisplay display, Context& main) {
  Gate gate;
  bool controllable = gate.Init(display);
  Check(controllable, "D3D11 exposes a controllable real GPU fence");
  if (!controllable) return;
  std::array<HANDLE, 8> signals{};
  for (HANDLE& signal : signals) {
    signal = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!signal) {
      Check(false, "allocate the bounded worker rendezvous");
      for (HANDLE owned : signals) if (owned) CloseHandle(owned);
      return;
    }
  }
  auto& [draw_ready, draw_start, draw_done, draw_finish, native_ready, native_start,
          native_created, native_done] = signals;
  // Context/driver startup is outside the GPU-gate timing window. Once both
  // contexts are current, only ordinary submission and synchronization remain.
  std::thread other([&] {
    Context context;
    bool ok = context.Init(display);
    Check(ok, "another thread has an independent ES2 context");
    SetEvent(draw_ready);
    bool start = WaitForSingleObject(draw_start, 5000) == WAIT_OBJECT_0;
    Check(start, "start the prepared GL worker");
    EGLSyncKHR early = EGL_NO_SYNC_KHR;
    if (ok && start) { Clear(0, 1); early = create_sync(display, EGL_SYNC_FENCE_KHR, nullptr); }
    Check(early != EGL_NO_SYNC_KHR, "the other context submits while the first fence is pending");
    if (early) Check(destroy_sync(display, early), "destroying a pending sync does not await GPU completion");
    SetEvent(draw_done);
    WaitForSingleObject(draw_finish, 5000);
    if (ok && start) Check(Pixel(0, 255), "the second context retains its framebuffer and state");
  });
  std::thread native([&] {
    Context context;
    bool ok = context.Init(display);
    Check(ok, "native producer context is prepared");
    SetEvent(native_ready);
    bool start = WaitForSingleObject(native_start, 5000) == WAIT_OBJECT_0;
    EGLSyncKHR value = ok && start ? create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr) : EGL_NO_SYNC_KHR;
    Check(value != EGL_NO_SYNC_KHR, "native fence creation succeeds");
    EGLint condition = 0;
    Check(value && attrib(display, value, EGL_SYNC_CONDITION_KHR, &condition) && condition == EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR,
          "a newly created native fence reports PRIOR_COMMANDS_COMPLETE");
    SetEvent(native_created);
    int fd = value ? duplicate(display, value) : -1;
    SetEvent(native_done);
    Check(fd >= 0, "native export returns an owned descriptor after completion");
    if (fd >= 0) {
      int alias = dup(fd);
      pollfd records[2]{{fd, POLLIN, 0}, {alias, POLLIN, 0}};
      Check(alias >= 0 && poll(records, 2, 5000) == 2 && (records[0].revents & POLLIN) && (records[1].revents & POLLIN),
            "duplicated native fence fds preserve completed readiness");
      close(alias); close(fd);
    }
    if (value) destroy_sync(display, value);
  });
  bool prepared = WaitForSingleObject(draw_ready, 5000) == WAIT_OBJECT_0 &&
                  WaitForSingleObject(native_ready, 5000) == WAIT_OBJECT_0;
  Check(prepared, "both worker contexts are initialized before the GPU gate");
  bool queued = prepared && gate.QueueWait();
  Check(queued, "the isolated test GPU gate waits for a different device's pending work");
  if (!queued) {
    gate.Signal(); SetEvent(draw_start); SetEvent(native_start); SetEvent(draw_finish);
    other.join(); native.join();
    for (HANDLE signal : signals) CloseHandle(signal);
    return;
  }
  Release release(gate);
  int creation_error = EGL_SUCCESS;
  auto abandoned = gesher::bionic::gpu::CreateCurrentFence(display, &creation_error);
  std::weak_ptr<gesher::NativeFence> retiring = abandoned;
  abandoned.reset();
  Check(!retiring.expired(), "a destroyed pending completion retains its event and device");
  Clear(1, 0);
  EGLSyncKHR sync = create_sync(display, EGL_SYNC_FENCE_KHR, nullptr);
  Check(sync && !release.watchdog.load(), "ES2 KHR fence creation returns before GPU completion");
  EGLint status = 0;
  Check(sync && attrib(display, sync, EGL_SYNC_STATUS_KHR, &status) && status == EGL_UNSIGNALED_KHR &&
        client_wait(display, sync, 0, 0) == EGL_TIMEOUT_EXPIRED_KHR, "pending KHR fence reports UNSIGNALED and a zero-timeout expires");
  Check(sync && wait_sync(display, sync, 0) == EGL_TRUE && !release.watchdog.load(), "eglWaitSyncKHR returns while the local GPU fence is pending");
  SetEvent(draw_start);
  Check(WaitForSingleObject(draw_done, 500) == WAIT_OBJECT_0 && !release.watchdog.load(),
        "GPU completion does not hold ANGLE's global GL mutex");
  SetEvent(native_start);
  Check(WaitForSingleObject(native_created, 500) == WAIT_OBJECT_0,
        "native sync creation returns while GPU work is pending");
  Check(WaitForSingleObject(native_done, 20) == WAIT_TIMEOUT,
        "native fd export awaits genuine GPU completion");
  main.Current();
  Clear(1, 0);
  Check(!release.watchdog.load(), "another context can submit while native export waits");
  release.Now();
  Check(sync && client_wait(display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 5'000'000'000ULL) == EGL_CONDITION_SATISFIED_KHR,
        "client wait observes the real producer's completion");
  Check(!release.watchdog.load(), "no GPU gate required the watchdog");
  const ULONGLONG retire_deadline = GetTickCount64() + 1500;
  while (!retiring.expired() && GetTickCount64() < retire_deadline) Sleep(1);
  Check(retiring.expired(), "GPU completion retires the abandoned event/device without a caller wait");
  Check(Pixel(255, 0), "the first context retains its framebuffer and state");
  SetEvent(draw_finish);
  other.join(); native.join();
  if (sync) destroy_sync(display, sync);
  for (HANDLE signal : signals) CloseHandle(signal);
}

void Validation(EGLDisplay display, Context& context) {
  EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
  Check(sync != EGL_NO_SYNC_KHR, "validation fixture has a native sync");
  if (!sync) return;
  int fd = duplicate(display, sync);
  const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
  EGLSyncKHR imported = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
  EGLint condition = 0;
  Check(imported && attrib(display, imported, EGL_SYNC_CONDITION_KHR, &condition) && condition == EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID,
        "an imported native fence reports NATIVE_FENCE_SIGNALED_ANDROID");
  if (imported) destroy_sync(display, imported);
  auto get_error = Gl<decltype(&eglGetError)>("eglGetError");
  Check(client_wait(display, sync, 0x4000, 0) == EGL_FALSE && get_error() == EGL_BAD_PARAMETER,
        "invalid client-wait flags report the precise EGL error");
  int invalid = open("/dev/null", O_RDONLY);
  const EGLint bad[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, invalid, EGL_NONE};
  Check(create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, bad) == EGL_NO_SYNC_KHR && get_error() == EGL_BAD_ATTRIBUTE,
        "an ordinary readable descriptor is not accepted as a native fence");
  Check(fcntl(invalid, F_GETFD) == -1, "rejected import still consumes its descriptor");
  int no_context_fd = duplicate(display, sync);
  const EGLint no_context[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, no_context_fd, EGL_NONE};
  Gl<decltype(&eglMakeCurrent)>("eglMakeCurrent")(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  Check(create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, no_context) == EGL_NO_SYNC_KHR && get_error() == EGL_BAD_MATCH &&
        fcntl(no_context_fd, F_GETFD) == -1, "import requires a current context and consumes fd on failure");
  context.Current();
  int stream_fd = duplicate(display, sync);
  FILE* stream = stream_fd >= 0 ? fdopen(stream_fd, "rb") : nullptr;
  Check(stream != nullptr, "a native descriptor can be owned by a FILE");
  if (stream) {
    Check(fclose(stream) == 0 && !gesher::LookupNativeFence(stream_fd),
          "CRT stream close retires native descriptor metadata");
    int replacement = open("/dev/null", O_RDONLY);
    if (replacement >= 0 && replacement != stream_fd) {
      Check(dup2(replacement, stream_fd) == stream_fd, "reuse the retired stream descriptor");
      close(replacement);
    }
    Check(replacement >= 0 && !gesher::LookupNativeFence(stream_fd),
          "an ordinary file reusing the fd is not mistaken for the closed fence");
    if (replacement >= 0) close(stream_fd);
  } else if (stream_fd >= 0) close(stream_fd);
  int reopen_fd = duplicate(display, sync);
  stream = reopen_fd >= 0 ? fdopen(reopen_fd, "rb") : nullptr;
  Check(stream != nullptr, "prepare a native descriptor stream for reopen");
  if (stream) {
    FILE* reopened = freopen("/dev/null", "rb", stream);
    Check(reopened != nullptr && !gesher::LookupNativeFence(reopen_fd),
          "CRT stream reopen replaces native descriptor metadata");
    if (reopened) fclose(reopened);
  } else if (reopen_fd >= 0) close(reopen_fd);
  Check(destroy_sync(EGL_NO_DISPLAY, sync) == EGL_FALSE && get_error() == EGL_BAD_DISPLAY,
        "wrong-display destroy fails without retiring a valid sync");
  Check(destroy_sync(display, sync) == EGL_TRUE, "the valid display still owns that sync");

  HANDLE ordinary = CreateEventW(nullptr, TRUE, TRUE, nullptr), read_only = nullptr;
  DuplicateHandle(GetCurrentProcess(), ordinary, GetCurrentProcess(), &read_only, SYNCHRONIZE | 1, FALSE, 0);
  Check(gesher::AdoptNativeFence(read_only, gesher::kTransferredCompletedFence) == -1,
        "transfer metadata cannot turn an unrelated event into a completed native fence");
  HANDLE untyped = nullptr;
  DuplicateHandle(GetCurrentProcess(), ordinary, GetCurrentProcess(), &untyped, SYNCHRONIZE | 1, FALSE, 0);
  Check(gesher::AdoptNativeFence(untyped, 0) == -1,
        "a native fence adoption requires its exact completed transport type");
  CloseHandle(ordinary);

  // Actual Windows waits forbid duplicate HANDLEs; duplicate POSIX eventfds
  // exercise the pending alias path while native exported fds are completed.
  int event = eventfd(0, 0), alias = dup(event);
  std::thread signal([event] { Sleep(20); uint64_t value = 1; write(event, &value, sizeof(value)); });
  pollfd aliases[2]{{event, POLLIN, 0}, {alias, POLLIN, 0}};
  Check(poll(aliases, 2, 2000) == 2 && (aliases[0].revents & POLLIN) && (aliases[1].revents & POLLIN),
        "pending duplicated descriptors wait once and report every poll slot");
  signal.join(); close(event); close(alias);
}

int Child(int argc, char** argv) {
  if (argc != 6) return 2;
  HANDLE owned = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(argv[2], nullptr, 0)));
  uint64_t metadata = std::strtoull(argv[3], nullptr, 0);
  HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(argv[4], nullptr, 0)));
  HANDLE proceed = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(argv[5], nullptr, 0)));
  int fd = gesher::AdoptNativeFence(owned, metadata);
  Check(fd >= 0, "child adopts the transferred native fence object");
  pollfd record{fd, POLLIN, 0};
  if (fd >= 0) Check(poll(&record, 1, 0) == 1, "only completed native fences are exposed across processes");
  SetEvent(ready); CloseHandle(ready);
  Check(WaitForSingleObject(proceed, 5000) == WAIT_OBJECT_0, "sender has retired all source descriptor references");
  CloseHandle(proceed);
  Check(fd >= 0 && poll(&record, 1, 5000) == 1 && (record.revents & POLLIN), "child observes completion after sender closes its fd");
  if (fd >= 0) close(fd);
  return failures.load() ? 1 : 0;
}

void Transfer(EGLDisplay display) {
  Clear(0, 1);
  int error = EGL_SUCCESS;
  auto completion = gesher::bionic::gpu::CreateCurrentFence(display, &error);
  Check(completion != nullptr, "transfer fixture records actual GPU completion");
  if (!completion) return;
  int fd = gesher::ExportNativeFence(completion);
  uintptr_t value = 0;
  uint64_t metadata = 0;
  std::shared_ptr<gesher::NativeFence> retained;
  bool described = fd >= 0 && gesher::TransferNativeFence(fd, &value, &metadata, &retained);
  Check(described, "native fd exports tagged handle metadata");
  if (!described) { if (fd >= 0) close(fd); return; }
  HANDLE handle = nullptr;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE proceed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE child_ready = nullptr;
  HANDLE child_proceed = nullptr;
  bool transferable = ready && proceed &&
      DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(value), GetCurrentProcess(), &handle, 0, TRUE, DUPLICATE_SAME_ACCESS) &&
      DuplicateHandle(GetCurrentProcess(), ready, GetCurrentProcess(), &child_ready, 0, TRUE, DUPLICATE_SAME_ACCESS) &&
      DuplicateHandle(GetCurrentProcess(), proceed, GetCurrentProcess(), &child_proceed, 0, TRUE, DUPLICATE_SAME_ACCESS);
  Check(transferable, "prepare checked inheritable completion and rendezvous handles");
  if (!transferable) {
    for (HANDLE owned : {handle, ready, proceed, child_ready, child_proceed}) if (owned) CloseHandle(owned);
    close(fd);
    return;
  }
  char path[MAX_PATH]{};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  char command[2 * MAX_PATH + 100]{};
  std::snprintf(command, sizeof(command), "\"%s\" --child %llu %llu %llu %llu", path,
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)),
      static_cast<unsigned long long>(metadata), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(child_ready)),
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(child_proceed)));
  STARTUPINFOA startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  bool started = CreateProcessA(nullptr, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) != FALSE;
  Check(started, "start a separate native-fence receiver process");
  CloseHandle(handle); CloseHandle(child_ready); CloseHandle(child_proceed); close(fd); retained.reset(); completion.reset();
  if (started) {
    bool acquired = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
    Check(acquired, "receiver owns its transferred handle after sender fd retirement");
    SetEvent(proceed);
    bool finished = WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0;
    Check(finished, "receiver finishes within its deadline");
    if (!finished) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 1000);
    }
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    Check(code == 0, "cross-process native fence readiness and ownership pass");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
  CloseHandle(ready);
  CloseHandle(proceed);
}
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return Child(argc, argv);
  gesher::bionic::gl::RegisterNativeBufferEgl(Add);
  create_sync = reinterpret_cast<Create>(entries.at("eglCreateSyncKHR"));
  destroy_sync = reinterpret_cast<Destroy>(entries.at("eglDestroySyncKHR"));
  wait_sync = reinterpret_cast<Wait>(entries.at("eglWaitSyncKHR"));
  client_wait = reinterpret_cast<ClientWait>(entries.at("eglClientWaitSyncKHR"));
  attrib = reinterpret_cast<Attrib>(entries.at("eglGetSyncAttribKHR"));
  duplicate = reinterpret_cast<Duplicate>(entries.at("eglDupNativeFenceFDANDROID"));
  EGLDisplay display = gesher_egl_default_display();
  bool abi = display != EGL_NO_DISPLAY && gesher_egl_proc("gesherAngleD3D11ContextV1") && gesher_egl_proc("gesherAngleEglErrorV1");
  Check(abi,
        "this package contains the current ANGLE synchronization ABI");
  if (!abi) return 1;
  Context context;
  Check(context.Init(display), "an ES2 context is current");
  if (!context.context) return 1;
  PendingEgl(display, context);
  Validation(display, context);
  Transfer(display);
  std::printf("egl_fence_test %s\n", failures.load() ? "FAILED" : "all passed");
  return failures.load() ? 1 : 0;
}
