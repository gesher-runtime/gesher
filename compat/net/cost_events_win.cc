// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "cost_events_win.h"

#include <objbase.h>
#include <ocidl.h>

#include <atomic>

namespace gesher::net {

const IID kIidNetworkConnectionCostEvents = {
    0xDCB0000B, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

namespace {

// netlistmgr.h: CLSID_NetworkListManager and IID_INetworkCostManager.
const CLSID kClsidNetworkListManager = {
    0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
const IID kIidNetworkCostManager = {
    0xDCB00008, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

class Sink final : public ConnectionCostEvents {
 public:
  explicit Sink(HANDLE event) : event_(event) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == kIidNetworkConnectionCostEvents) {
      *object = static_cast<ConnectionCostEvents*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  STDMETHODIMP ConnectionCostChanged(GUID, DWORD) override {
    SetEvent(event_);
    return S_OK;
  }
  STDMETHODIMP ConnectionDataPlanStatusChanged(GUID) override {
    SetEvent(event_);
    return S_OK;
  }

 private:
  HANDLE event_;
  std::atomic<ULONG> refs_{1};
};

}  // namespace

struct CostWatch {
  // The caller and the worker each own one reference once CreateThread succeeds.
  // A timed-out caller can leave while COM initialization is still running.
  std::atomic<ULONG> refs{1};
  HANDLE event = nullptr;
  HANDLE stop = nullptr;
  HANDLE ready = nullptr;
  HANDLE thread = nullptr;
  HRESULT result = E_FAIL;

  void Release() {
    if (--refs != 0) return;
    if (thread != nullptr) CloseHandle(thread);
    if (stop != nullptr) CloseHandle(stop);
    if (ready != nullptr) CloseHandle(ready);
    if (event != nullptr) CloseHandle(event);
    delete this;
  }
};

namespace {

DWORD WINAPI WatchThread(void* argument) {
  CostWatch* watch = static_cast<CostWatch*>(argument);
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IConnectionPoint* point = nullptr;
  DWORD cookie = 0;
  ConnectionCostEvents* sink = nullptr;
  HRESULT hr = SUCCEEDED(init) ? S_OK : init;
  if (SUCCEEDED(hr)) {
    IUnknown* manager = nullptr;
    hr = CoCreateInstance(kClsidNetworkListManager, nullptr, CLSCTX_ALL, kIidNetworkCostManager,
                          reinterpret_cast<void**>(&manager));
    IConnectionPointContainer* container = nullptr;
    if (SUCCEEDED(hr)) {
      hr = manager->QueryInterface(IID_IConnectionPointContainer,
                                   reinterpret_cast<void**>(&container));
    }
    if (SUCCEEDED(hr)) hr = container->FindConnectionPoint(kIidNetworkConnectionCostEvents, &point);
    if (SUCCEEDED(hr)) {
      sink = NewConnectionCostSink(watch->event);
      hr = point->Advise(sink, &cookie);
    }
    if (container != nullptr) container->Release();
    if (manager != nullptr) manager->Release();
  }
  watch->result = hr;
  SetEvent(watch->ready);
  if (SUCCEEDED(hr)) {
    WaitForSingleObject(watch->stop, INFINITE);
    point->Unadvise(cookie);
  }
  if (sink != nullptr) sink->Release();
  if (point != nullptr) point->Release();
  if (SUCCEEDED(init)) CoUninitialize();
  watch->Release();
  return 0;
}

}  // namespace

ConnectionCostEvents* NewConnectionCostSink(HANDLE event) { return new Sink(event); }

CostWatch* StartCostWatch(HANDLE event, HRESULT* hr_out) {
  HRESULT ignored;
  HRESULT& hr = hr_out != nullptr ? *hr_out : ignored;
  CostWatch* watch = new CostWatch;
  // A failed StartCostWatch lets the caller close its event even if the worker is
  // still returning from COM. Keep an owned handle for any late callback.
  if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &watch->event,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) {
    hr = HRESULT_FROM_WIN32(GetLastError());
    watch->Release();
    return nullptr;
  }
  watch->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (watch->stop != nullptr) watch->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (watch->stop != nullptr && watch->ready != nullptr) {
    ++watch->refs;
    watch->thread = CreateThread(nullptr, 0, WatchThread, watch, 0, nullptr);
    if (watch->thread == nullptr) watch->Release();
  }
  if (watch->thread == nullptr) {
    hr = HRESULT_FROM_WIN32(GetLastError());
    StopCostWatch(watch);
    return nullptr;
  }
  if (WaitForSingleObject(watch->ready, 30000) != WAIT_OBJECT_0) {
    hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    // The worker's reference keeps the events alive until it has unadvised and exited COM.
    SetEvent(watch->stop);
    watch->Release();
    return nullptr;
  }
  hr = watch->result;
  if (FAILED(hr)) {
    StopCostWatch(watch);
    return nullptr;
  }
  return watch;
}

void StopCostWatch(CostWatch* watch) {
  if (watch == nullptr) return;
  if (watch->thread != nullptr) {
    SetEvent(watch->stop);
    WaitForSingleObject(watch->thread, INFINITE);
  }
  watch->Release();
}

}  // namespace gesher::net
