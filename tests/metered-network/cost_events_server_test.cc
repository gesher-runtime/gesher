// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Real Windows Server sink and failed-start lifecycle contracts. An unavailable
// cost backend is accepted only after an independent SDK probe proves the exact
// missing-backend signature observed on Server 2022/2025. All other failures fail
// this test. The separate required Windows 11 client job always runs the unchanged
// cost_events_windows_test.cc and requires successful real subscription.
#include "../../compat/net/cost_events_win.h"

#include <netlistmgr.h>
#include <objbase.h>
#include <objidl.h>
#include <ocidl.h>
#include <olectl.h>

#include <atomic>
#include <cstdio>

namespace {
int checks = 0;
int failures = 0;

void Check(bool condition, const char* label) {
  ++checks;
  if (!condition) ++failures;
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", label);
}

void Result(const char* label, HRESULT hr) {
  std::printf("info %s: hr 0x%08lx\n", label, static_cast<unsigned long>(hr));
}

// Use the Windows SDK's interface and IID, independently of Gesher's declaration.
class SdkSink final : public INetworkConnectionCostEvents {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != __uuidof(INetworkConnectionCostEvents)) return E_NOINTERFACE;
    *object = static_cast<INetworkConnectionCostEvents*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
  }
  STDMETHODIMP ConnectionCostChanged(GUID, DWORD) override { return S_OK; }
  STDMETHODIMP ConnectionDataPlanStatusChanged(GUID) override { return S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
};

struct Evidence {
  bool nlm_and_point_ready = false;
  bool proxy_ready = false;
  HRESULT cost_manager = E_FAIL;
  HRESULT sdk_advise = E_FAIL;
  unsigned connections = 0;
  bool enumeration_complete = false;
  bool all_cost_interfaces_absent = true;
};

bool BackendAbsent(const Evidence& evidence) {
  return evidence.nlm_and_point_ready && evidence.proxy_ready
      && (evidence.cost_manager == REGDB_E_CLASSNOTREG || evidence.cost_manager == E_NOINTERFACE)
      && evidence.sdk_advise == CONNECT_E_CANNOTCONNECT
      && evidence.enumeration_complete && evidence.connections != 0
      && evidence.all_cost_interfaces_absent;
}

Evidence ObserveBackend() {
  Evidence evidence;
  CLSID proxy = {};
  HRESULT hr = CoGetPSClsid(__uuidof(INetworkConnectionCostEvents), &proxy);
  if (SUCCEEDED(hr)) {
    IPSFactoryBuffer* factory = nullptr;
    hr = CoGetClassObject(proxy, CLSCTX_INPROC_SERVER, nullptr, IID_IPSFactoryBuffer,
                          reinterpret_cast<void**>(&factory));
    evidence.proxy_ready = SUCCEEDED(hr) && factory != nullptr;
    if (factory != nullptr) factory->Release();
  }
  Result("SDK cost-event proxy activation", hr);

  INetworkCostManager* costs = nullptr;
  evidence.cost_manager = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                            __uuidof(INetworkCostManager),
                                            reinterpret_cast<void**>(&costs));
  Result("SDK cost manager activation", evidence.cost_manager);
  if (costs != nullptr) costs->Release();

  IConnectionPointContainer* container = nullptr;
  hr = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                        IID_IConnectionPointContainer, reinterpret_cast<void**>(&container));
  Result("SDK NLM container", hr);
  if (FAILED(hr) || container == nullptr) return evidence;

  INetworkListManager* manager = nullptr;
  hr = container->QueryInterface(__uuidof(INetworkListManager),
                                  reinterpret_cast<void**>(&manager));
  IEnumNetworkConnections* connections = nullptr;
  if (SUCCEEDED(hr) && manager != nullptr) hr = manager->GetNetworkConnections(&connections);
  Result("SDK connection enumeration", hr);
  bool nlm_ready = SUCCEEDED(hr) && connections != nullptr;
  if (nlm_ready) {
    // Complete enumeration is required for the absence branch. A bound prevents
    // a broken enumerator from hanging CI; reaching it is a failure, not absence.
    for (unsigned index = 0; index <= 64; ++index) {
      INetworkConnection* connection = nullptr;
      ULONG fetched = 0;
      hr = connections->Next(1, &connection, &fetched);
      if (hr == S_FALSE && fetched == 0 && connection == nullptr) {
        evidence.enumeration_complete = true;
        break;
      }
      if (hr != S_OK || fetched != 1 || connection == nullptr || index == 64) {
        Result("SDK incomplete connection enumeration", hr);
        if (connection != nullptr) connection->Release();
        break;
      }
      ++evidence.connections;
      INetworkConnectionCost* cost = nullptr;
      HRESULT cost_hr = connection->QueryInterface(__uuidof(INetworkConnectionCost),
                                                    reinterpret_cast<void**>(&cost));
      Result("SDK real connection cost interface", cost_hr);
      evidence.all_cost_interfaces_absent = evidence.all_cost_interfaces_absent
          && cost_hr == E_NOINTERFACE && cost == nullptr;
      if (cost != nullptr) cost->Release();
      connection->Release();
    }
  }
  if (connections != nullptr) connections->Release();
  if (manager != nullptr) manager->Release();

  IConnectionPoint* point = nullptr;
  hr = container->FindConnectionPoint(__uuidof(INetworkConnectionCostEvents), &point);
  Result("SDK cost connection point", hr);
  if (SUCCEEDED(hr) && point != nullptr) {
    IID outgoing = {};
    hr = point->GetConnectionInterface(&outgoing);
    evidence.nlm_and_point_ready = nlm_ready && hr == S_OK
        && outgoing == __uuidof(INetworkConnectionCostEvents);
    auto* sink = new SdkSink;
    DWORD cookie = 0;
    evidence.sdk_advise = point->Advise(sink, &cookie);
    Result("independent SDK Advise", evidence.sdk_advise);
    if (evidence.sdk_advise == S_OK) {
      Check(cookie != 0, "SDK subscription returns a cookie");
      Check(point->Unadvise(cookie) == S_OK, "SDK subscription is released");
    } else {
      Check(cookie == 0, "failed SDK subscription does not return a cookie");
    }
    sink->Release();
    point->Release();
  }
  container->Release();
  std::printf("info SDK connections: %u; complete: %d; all cost interfaces absent: %d\n",
              evidence.connections, evidence.enumeration_complete ? 1 : 0,
              evidence.all_cost_interfaces_absent ? 1 : 0);
  return evidence;
}

void CheckEvidencePredicate() {
  Evidence evidence;
  evidence.nlm_and_point_ready = true;
  evidence.proxy_ready = true;
  evidence.cost_manager = REGDB_E_CLASSNOTREG;
  evidence.sdk_advise = CONNECT_E_CANNOTCONNECT;
  evidence.connections = 1;
  evidence.enumeration_complete = true;
  Check(BackendAbsent(evidence), "only the complete known absence signature is accepted");
  evidence.cost_manager = E_ACCESSDENIED;
  Check(!BackendAbsent(evidence), "cost-manager access failure is not absence");
  evidence.cost_manager = E_NOINTERFACE;
  Check(BackendAbsent(evidence), "confirmed missing cost interface permits the same absence signature");
  evidence.sdk_advise = E_NOINTERFACE;
  Check(!BackendAbsent(evidence), "arbitrary Advise failure is not absence");
  evidence.sdk_advise = CONNECT_E_CANNOTCONNECT;
  evidence.connections = 0;
  Check(!BackendAbsent(evidence), "no real connection cannot prove backend absence");
  evidence.connections = 1;
  evidence.enumeration_complete = false;
  Check(!BackendAbsent(evidence), "truncated or failed enumeration is not absence");
  evidence.enumeration_complete = true;
  evidence.all_cost_interfaces_absent = false;
  Check(!BackendAbsent(evidence), "one available or unexpectedly failed cost interface blocks absence");
  evidence.all_cost_interfaces_absent = true;
  evidence.proxy_ready = false;
  Check(!BackendAbsent(evidence), "a broken COM proxy is not backend absence");
  evidence.proxy_ready = true;
  evidence.nlm_and_point_ready = false;
  Check(!BackendAbsent(evidence), "an unavailable NLM or wrong outgoing IID is not backend absence");
}

void CheckSink(HANDLE event) {
  auto* sink = gesher::net::NewConnectionCostSink(event);
  Check(sink != nullptr, "production sink is created");
  void* raw = nullptr;
  HRESULT hr = sink->QueryInterface(__uuidof(INetworkConnectionCostEvents), &raw);
  Check(hr == S_OK && raw != nullptr, "production sink exposes the Windows SDK event IID");
  if (raw != nullptr) {
    auto* sdk = static_cast<INetworkConnectionCostEvents*>(raw);
    GUID connection = {};
    Check(sdk->ConnectionCostChanged(connection, NLM_CONNECTION_COST_FIXED) == S_OK,
          "SDK cost callback reaches the production sink");
    Check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "cost callback signals the real event");
    ResetEvent(event);
    Check(sdk->ConnectionDataPlanStatusChanged(connection) == S_OK,
          "SDK data-plan callback reaches the production sink");
    Check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "data-plan callback signals the real event");
    ResetEvent(event);
    sdk->Release();
  }
  raw = nullptr;
  Check(sink->QueryInterface(IID_IUnknown, &raw) == S_OK && raw == static_cast<void*>(sink),
        "production sink has canonical COM identity");
  if (raw != nullptr) static_cast<IUnknown*>(raw)->Release();
  raw = nullptr;
  Check(sink->QueryInterface(IID_IConnectionPoint, &raw) == E_NOINTERFACE && raw == nullptr,
        "production sink rejects unrelated interfaces");
  Check(sink->AddRef() == 2, "production sink owns one remaining reference");
  Check(sink->Release() == 1, "production sink drops the extra reference");
  Check(sink->Release() == 0, "production sink releases its last reference");
}

bool FailedStart(HANDLE event) {
  HRESULT hr = E_FAIL;
  gesher::net::CostWatch* watch = gesher::net::StartCostWatch(event, &hr);
  const bool expected = watch == nullptr && hr == CONNECT_E_CANNOTCONNECT;
  Check(expected,
        "production fails truthfully with the independently observed unavailable backend");
  gesher::net::StopCostWatch(watch);
  return expected;
}

void CheckFailedStartCleanup(HANDLE event) {
  // Warm COM's process-wide resources before comparing handle counts.
  if (!FailedStart(event) || !FailedStart(event)) return;
  DWORD before = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "failed-start handle baseline");
  for (int round = 0; round != 32; ++round) {
    if (!FailedStart(event)) break;
  }
  DWORD after = 0;
  bool counted = false;
  for (int attempt = 0; attempt != 50; ++attempt) {
    counted = GetProcessHandleCount(GetCurrentProcess(), &after) != 0;
    if (!counted || after <= before) break;
    Sleep(10);
  }
  std::printf("info failed-start handles: before %lu after %lu\n",
              static_cast<unsigned long>(before), static_cast<unsigned long>(after));
  Check(counted && after <= before, "repeated unavailable starts reclaim their owned handles");
  Check(SetEvent(event) != 0 && WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
        "failed initialization preserves the caller's event handle");
  ResetEvent(event);
  HRESULT hr = S_OK;
  auto* invalid = gesher::net::StartCostWatch(nullptr, &hr);
  Check(invalid == nullptr && hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
        "invalid caller handle fails explicitly before COM initialization");
  gesher::net::StopCostWatch(invalid);
}
}  // namespace

int main() {
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Check(SUCCEEDED(init), "test enters the multithreaded COM apartment");
  if (FAILED(init)) return 1;
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Check(event != nullptr, "caller event is created");
  if (event == nullptr) {
    CoUninitialize();
    return 1;
  }
  CheckEvidencePredicate();
  CheckSink(event);
  Evidence evidence = ObserveBackend();
  Check(evidence.nlm_and_point_ready, "independent NLM and outgoing interface are valid");
  if (evidence.sdk_advise == S_OK) {
    for (int round = 0; round != 2; ++round) {
      HRESULT hr = E_FAIL;
      auto* watch = gesher::net::StartCostWatch(event, &hr);
      Check(watch != nullptr && hr == S_OK, "available SDK backend requires real production subscription");
      gesher::net::StopCostWatch(watch);
    }
  } else {
    bool absent = BackendAbsent(evidence);
    Check(absent, "unavailable branch requires complete independent SDK evidence");
    if (absent) CheckFailedStartCleanup(event);
  }
  gesher::net::StopCostWatch(nullptr);
  CloseHandle(event);
  CoUninitialize();
  std::printf("cost-events-server %s: %d checks, %d failures\n",
              failures == 0 ? "PASSED" : "FAILED", checks, failures);
  return failures == 0 ? 0 : 1;
}
