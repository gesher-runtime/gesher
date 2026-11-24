// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows host test of compat/net/cost_events_win.cc (no UI, no setting changed): the
// sink Gesher advises to the Network List Manager sets the watcher's event for a cost or
// a data plan change of a connection, answers only its own interface, and subscribing and
// unsubscribing against the real Network List Manager works. Toggling the metered setting
// is the manual probe in HUMAN_TEST.md.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
//      compat\net\cost_events_win.cc tests\metered-network\cost_events_windows_test.cc
//      /Fe:cost-events-test.exe ole32.lib
#include "../../compat/net/cost_events_win.h"

#include <objbase.h>

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

bool Signaled(HANDLE event) { return WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }

}  // namespace

int main() {
  using gesher::net::ConnectionCostEvents;
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Check(event != nullptr, "event created");

  // The sink, called as the Network List Manager calls it.
  ConnectionCostEvents* sink = gesher::net::NewConnectionCostSink(event);
  Check(sink != nullptr, "a sink is made");
  const GUID connection = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  Check(!Signaled(event), "quiet until Windows says something");
  Check(sink->ConnectionCostChanged(connection, 0x2) == S_OK, "ConnectionCostChanged returns S_OK");
  Check(Signaled(event), "a cost change wakes the watcher");
  ResetEvent(event);
  Check(sink->ConnectionDataPlanStatusChanged(connection) == S_OK,
        "ConnectionDataPlanStatusChanged returns S_OK");
  Check(Signaled(event), "a data plan change wakes the watcher");
  ResetEvent(event);

  // COM identity: IUnknown and INetworkConnectionCostEvents only.
  void* other = nullptr;
  Check(sink->QueryInterface(gesher::net::kIidNetworkConnectionCostEvents, &other) == S_OK &&
            other == static_cast<void*>(sink),
        "QueryInterface(INetworkConnectionCostEvents) is the sink");
  if (other != nullptr) static_cast<IUnknown*>(other)->Release();
  other = nullptr;
  Check(sink->QueryInterface(IID_IUnknown, &other) == S_OK, "QueryInterface(IUnknown)");
  if (other != nullptr) static_cast<IUnknown*>(other)->Release();
  other = nullptr;
  const IID unrelated = {0x0badf00d, 0x1111, 0x2222, {3, 3, 4, 4, 5, 5, 6, 6}};
  Check(sink->QueryInterface(unrelated, &other) == E_NOINTERFACE && other == nullptr,
        "an unrelated interface is refused");
  ULONG refs = sink->AddRef();
  Check(refs == 2, "one reference to start, then AddRef is 2");
  Check(sink->Release() == 1, "Release back to 1");
  Check(sink->Release() == 0, "the last Release frees it");

  // Against the real Network List Manager: subscribe, then unsubscribe, twice.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  for (int round = 1; round <= 2; ++round) {
    HRESULT hr = E_FAIL;
    gesher::net::CostWatch* watch = gesher::net::StartCostWatch(event, &hr);
    std::printf("info round %d: StartCostWatch hr 0x%08lx\n", round, static_cast<unsigned long>(hr));
    Check(watch != nullptr && hr == S_OK, "subscribed to the Network List Manager's cost events");
    Check(!Signaled(event), "subscribing alone does not wake the watcher");
    gesher::net::StopCostWatch(watch);
    Check(true, "unsubscribed");
  }
  gesher::net::StopCostWatch(nullptr);
  Check(true, "stopping nothing is fine");
  CoUninitialize();
  CloseHandle(event);
  std::printf("cost-events %s: %d checks, %d failures\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
