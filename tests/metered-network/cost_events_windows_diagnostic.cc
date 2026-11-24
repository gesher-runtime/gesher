// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Isolated Windows SDK diagnostic for NLM subscription failures. This neither changes
// network settings nor replaces cost_events_windows_test.cc: it prints the SDK IIDs,
// proxy registration, connection-point IID and QueryInterface requests so that a
// failed real subscription can be investigated without weakening that strict test.
// The SDK sink can expose only connection events or both cost interfaces, as in
// Microsoft's Samples/NetworkCost/cpp/NetCostEventSink.cpp.
//
// cl /nologo /std:c++17 /EHsc /W4 /WX /DNOGDI /DWIN32_LEAN_AND_MEAN
//    /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 compat\net\cost_events_win.cc
//    tests\metered-network\cost_events_windows_diagnostic.cc
//    /Fe:cost-events-diagnostic.exe ole32.lib uuid.lib
#include "../../compat/net/cost_events_win.h"

#include <netlistmgr.h>
#include <objbase.h>
#include <objidl.h>
#include <ocidl.h>

#include <atomic>
#include <cstdio>

namespace {

void PrintResult(const char* label, HRESULT hr) {
  std::printf("diagnostic %s: hr 0x%08lx\n", label, static_cast<unsigned long>(hr));
}

void PrintGuid(const char* label, REFIID iid) {
  wchar_t text[40] = {};
  StringFromGUID2(iid, text, 40);
  std::printf("diagnostic %s: %ls\n", label, text);
}

void PrintProxy(REFIID iid, const char* label) {
  CLSID proxy = {};
  HRESULT hr = CoGetPSClsid(iid, &proxy);
  PrintResult(label, hr);
  if (SUCCEEDED(hr)) {
    PrintGuid("proxy CLSID", proxy);
    IPSFactoryBuffer* factory = nullptr;
    hr = CoGetClassObject(proxy, CLSCTX_INPROC_SERVER, nullptr, IID_IPSFactoryBuffer,
                          reinterpret_cast<void**>(&factory));
    std::printf("diagnostic %s IPSFactoryBuffer activation: hr 0x%08lx\n", label,
                static_cast<unsigned long>(hr));
    if (factory != nullptr) factory->Release();
  }
}

void PrintMachineCostActivation() {
  INetworkCostManager* manager = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                               __uuidof(INetworkCostManager),
                               reinterpret_cast<void**>(&manager));
  PrintResult("CoCreateInstance SDK INetworkCostManager", hr);
  if (manager != nullptr) manager->Release();
}

void PrintConnectionCosts(IConnectionPointContainer* container) {
  INetworkListManager* manager = nullptr;
  HRESULT hr = container->QueryInterface(__uuidof(INetworkListManager),
                                         reinterpret_cast<void**>(&manager));
  PrintResult("container QueryInterface SDK INetworkListManager", hr);
  IEnumNetworkConnections* connections = nullptr;
  if (SUCCEEDED(hr)) {
    hr = manager->GetNetworkConnections(&connections);
    PrintResult("GetNetworkConnections", hr);
  }
  if (SUCCEEDED(hr)) {
    // A bounded sample of real connections is enough to distinguish interface
    // acquisition from an unavailable cost provider; no settings are changed.
    for (int index = 0; index != 4; ++index) {
      INetworkConnection* connection = nullptr;
      ULONG fetched = 0;
      hr = connections->Next(1, &connection, &fetched);
      if (hr != S_OK || fetched == 0) {
        PrintResult("connection enumeration end", hr);
        if (connection != nullptr) connection->Release();
        break;
      }
      std::printf("diagnostic real connection %d\n", index + 1);
      INetworkConnectionCost* cost = nullptr;
      hr = connection->QueryInterface(__uuidof(INetworkConnectionCost),
                                       reinterpret_cast<void**>(&cost));
      PrintResult("connection QueryInterface SDK INetworkConnectionCost", hr);
      if (SUCCEEDED(hr)) {
        DWORD value = 0;
        hr = cost->GetCost(&value);
        PrintResult("connection GetCost", hr);
        if (SUCCEEDED(hr)) {
          std::printf("diagnostic connection cost flags: 0x%08lx\n",
                      static_cast<unsigned long>(value));
        }
      }
      if (cost != nullptr) cost->Release();
      connection->Release();
    }
  }
  if (connections != nullptr) connections->Release();
  if (manager != nullptr) manager->Release();
}

class SdkSink final : public INetworkConnectionCostEvents, public INetworkCostManagerEvents {
 public:
  explicit SdkSink(bool dual) : dual_(dual) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    const char* label = dual_ ? "SDK dual sink QueryInterface" : "SDK single sink QueryInterface";
    PrintGuid(label, iid);
    if (iid == IID_IUnknown || iid == __uuidof(INetworkConnectionCostEvents)) {
      *object = static_cast<INetworkConnectionCostEvents*>(this);
    } else if (dual_ && iid == __uuidof(INetworkCostManagerEvents)) {
      *object = static_cast<INetworkCostManagerEvents*>(this);
    }
    HRESULT hr = *object == nullptr ? E_NOINTERFACE : S_OK;
    if (SUCCEEDED(hr)) AddRef();
    PrintResult(label, hr);
    return hr;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP ConnectionCostChanged(GUID, DWORD) override { return S_OK; }
  STDMETHODIMP ConnectionDataPlanStatusChanged(GUID) override { return S_OK; }
  STDMETHODIMP CostChanged(DWORD, NLM_SOCKADDR*) override { return S_OK; }
  STDMETHODIMP DataPlanStatusChanged(NLM_SOCKADDR*) override { return S_OK; }

 private:
  const bool dual_;
  std::atomic<ULONG> refs_{1};
};

void Probe(IConnectionPoint* point, IUnknown* sink, const char* label) {
  std::printf("diagnostic begin %s\n", label);
  DWORD cookie = 0;
  HRESULT hr = point->Advise(sink, &cookie);
  PrintResult(label, hr);
  std::printf("diagnostic cookie: %lu\n", static_cast<unsigned long>(cookie));
  if (SUCCEEDED(hr)) PrintResult("Unadvise", point->Unadvise(cookie));
  std::printf("diagnostic end %s\n", label);
}

}  // namespace

int main() {
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  PrintResult("CoInitializeEx MTA", init);
  if (FAILED(init)) return 1;

  PrintGuid("SDK connection cost events IID", __uuidof(INetworkConnectionCostEvents));
  PrintGuid("production connection cost events IID", gesher::net::kIidNetworkConnectionCostEvents);
  const bool iid_matches =
      __uuidof(INetworkConnectionCostEvents) == gesher::net::kIidNetworkConnectionCostEvents;
  std::printf("diagnostic SDK/production IID match: %s\n", iid_matches ? "yes" : "NO");
  PrintProxy(__uuidof(INetworkConnectionCostEvents), "connection cost event proxy");
  PrintProxy(__uuidof(INetworkCostManagerEvents), "machine cost event proxy");
  PrintProxy(__uuidof(INetworkConnectionEvents), "connection event proxy");
  PrintMachineCostActivation();

  IConnectionPointContainer* container = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                               IID_IConnectionPointContainer,
                               reinterpret_cast<void**>(&container));
  PrintResult("CoCreateInstance connection-point container", hr);
  IConnectionPoint* point = nullptr;
  if (SUCCEEDED(hr)) {
    PrintConnectionCosts(container);
    hr = container->FindConnectionPoint(__uuidof(INetworkConnectionCostEvents), &point);
    PrintResult("FindConnectionPoint SDK connection cost events", hr);
  }
  if (SUCCEEDED(hr)) {
    IID outgoing = {};
    HRESULT interface_hr = point->GetConnectionInterface(&outgoing);
    PrintResult("GetConnectionInterface", interface_hr);
    if (SUCCEEDED(interface_hr)) PrintGuid("actual outgoing IID", outgoing);

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event != nullptr) {
      auto* production_sink = gesher::net::NewConnectionCostSink(event);
      void* sdk_interface = nullptr;
      HRESULT query_hr = production_sink->QueryInterface(__uuidof(INetworkConnectionCostEvents),
                                                         &sdk_interface);
      PrintResult("production sink QueryInterface with SDK IID", query_hr);
      if (sdk_interface != nullptr) {
        static_cast<INetworkConnectionCostEvents*>(sdk_interface)->Release();
      }
      Probe(point, production_sink, "production sink Advise");
      production_sink->Release();
      CloseHandle(event);
    } else {
      PrintResult("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    }

    for (int mode = 0; mode != 2; ++mode) {
      const bool dual = mode != 0;
      auto* sink = new SdkSink(dual);
      Probe(point, static_cast<INetworkConnectionCostEvents*>(sink),
            dual ? "SDK dual sink Advise" : "SDK single sink Advise");
      sink->Release();
    }
  }
  if (point != nullptr) point->Release();
  if (container != nullptr) container->Release();
  CoUninitialize();
  std::printf("diagnostic collection complete; the separate strict subscription test is required\n");
  return iid_matches ? 0 : 1;
}
