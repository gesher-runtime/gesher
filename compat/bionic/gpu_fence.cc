// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "gpu_interop.h"
#include "gpu_fence.h"
#include <gesher/egl_angle.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace gesher::bionic::gpu {
namespace {
using Microsoft::WRL::ComPtr;
using Operation = void(__stdcall*)(ID3D11Device*, ID3D11DeviceContext*, void*);
using Dispatch = int(__stdcall*)(Operation, void*);

Dispatch AngleDispatch() {
  static auto call = reinterpret_cast<Dispatch>(gesher_egl_proc("gesherAngleD3D11ContextV1"));
  return call;
}

int Error(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG
      ? EGL_CONTEXT_LOST : EGL_BAD_ALLOC;
}

struct Submission {
  ComPtr<ID3D11Device> device;
  HANDLE event = nullptr;
  HRESULT result = E_FAIL;
};

void __stdcall Submit(ID3D11Device* device, ID3D11DeviceContext*, void* argument) {
  auto& result = *static_cast<Submission*>(argument);
  result.device = device;
  // DXGI completion is available on older D3D11 drivers. EnqueueSetEvent
  // flushes and registers completion, without waiting for the GPU. It must
  // also run under ANGLE's lock because it touches the immediate command queue.
  ComPtr<IDXGIDevice2> dxgi;
  result.result = device->QueryInterface(__uuidof(IDXGIDevice2), reinterpret_cast<void**>(dxgi.GetAddressOf()));
  if (FAILED(result.result)) return;
  result.event = CreateNativeFenceEvent();
  if (!result.event) { result.result = E_OUTOFMEMORY; return; }
  result.result = dxgi->EnqueueSetEvent(result.event);
  if (FAILED(result.result)) { CloseHandle(result.event); result.event = nullptr; }
}

struct ServerWait {
  const std::shared_ptr<NativeFence>& fence;
  HRESULT result = E_FAIL;
};

void __stdcall QueueWait(ID3D11Device* device, ID3D11DeviceContext*, void* argument) {
  auto& wait = *static_cast<ServerWait*>(argument);
  wait.result = ServerWaitNativeFence(wait.fence, device);
}

}  // namespace

std::shared_ptr<NativeFence> CreateCurrentFence(void* display, int* egl_error) {
  *egl_error = EGL_SUCCESS;
  Submission submission;
  if (auto dispatch = AngleDispatch()) {
    if (!dispatch(Submit, &submission)) { *egl_error = EGL_CONTEXT_LOST; return nullptr; }
    if (FAILED(submission.result)) { *egl_error = Error(submission.result); return nullptr; }
  } else {
    // A previously installed ANGLE SDK cannot safely expose its immediate
    // context concurrently. Retain a correct synchronous compatibility path;
    // current packages include the dispatch and never take this branch.
    const Gl* gl = Functions();
    EGLAttrib egl_device = 0, d3d = 0;
    if (!gl || !gl->eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &egl_device) ||
        !gl->eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(egl_device), EGL_D3D11_DEVICE_ANGLE, &d3d) || !d3d) {
      *egl_error = EGL_BAD_ALLOC;
      return nullptr;
    }
    submission.device = reinterpret_cast<ID3D11Device*>(d3d);
    auto finish = reinterpret_cast<decltype(&::glFinish)>(gesher_egl_proc("glFinish"));
    if (!finish) { *egl_error = EGL_BAD_ALLOC; return nullptr; }
    finish();
    if (FAILED(submission.device->GetDeviceRemovedReason())) { *egl_error = EGL_CONTEXT_LOST; return nullptr; }
    submission.event = CreateNativeFenceEvent();
    if (!submission.event) { *egl_error = EGL_BAD_ALLOC; return nullptr; }
    // The REAL glFinish above has completed this work.
    if (!SetEvent(submission.event)) {
      CloseHandle(submission.event);
      *egl_error = EGL_BAD_ALLOC;
      return nullptr;
    }
  }
  auto fence = NativeFenceFromEvent(submission.device.Get(), submission.event);
  if (!fence) *egl_error = EGL_BAD_ALLOC;
  return fence;
}

bool WaitCurrentFence(const std::shared_ptr<NativeFence>& fence, int* egl_error) {
  *egl_error = EGL_SUCCESS;
  ServerWait wait{fence};
  if (auto dispatch = AngleDispatch()) {
    if (!dispatch(QueueWait, &wait)) { *egl_error = EGL_CONTEXT_LOST; return false; }
    if (FAILED(wait.result)) { *egl_error = wait.result == E_INVALIDARG ? EGL_BAD_MATCH : Error(wait.result); return false; }
    return true;
  }
  // Old SDKs only create already-completed local fences, and native imports
  // are completed too. Never turn an unexpected pending object into a CPU
  // block or silently accept a foreign dependency on the shared queue.
  if (NativeFenceStatus(fence) == FenceStatus::kSignaled) return true;
  *egl_error = EGL_BAD_MATCH;
  return false;
}

}  // namespace gesher::bionic::gpu
