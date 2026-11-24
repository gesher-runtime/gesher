// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows host test of compat/net/background_data_win.cc (no UI, no setting changed):
// with an internet connection (the Network List Manager says so) Windows answers 0 or 1
// for ConnectionCost.BackgroundDataUsageRestricted, the same twice; without one, unknown.
//
//   cl /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
//      compat\net\background_data_win.cc tests\metered-network\background_data_windows_test.cc
//      /Fe:background-data-test.exe ole32.lib
#include "../../compat/net/background_data_win.h"

#include <objbase.h>
#include <netlistmgr.h>

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// CLSID_NetworkListManager / IID_INetworkListManager (netlistmgr.h).
const CLSID kClsidNetworkListManager = {
    0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
const IID kIidNetworkListManager = {
    0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

int InternetPerNlm() {
  INetworkListManager* nlm = nullptr;
  if (FAILED(CoCreateInstance(kClsidNetworkListManager, nullptr, CLSCTX_ALL, kIidNetworkListManager,
                              reinterpret_cast<void**>(&nlm)))) {
    return -1;
  }
  VARIANT_BOOL connected = VARIANT_FALSE;
  HRESULT hr = nlm->get_IsConnectedToInternet(&connected);
  nlm->Release();
  return FAILED(hr) ? -1 : connected != VARIANT_FALSE ? 1 : 0;
}

}  // namespace

int main() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HRESULT hr = S_OK;
  int restricted = gesher::net::BackgroundDataRestricted(&hr);
  int internet = InternetPerNlm();
  std::printf("info BackgroundDataUsageRestricted: %d (hr 0x%08lx); NLM internet: %d\n", restricted,
              static_cast<unsigned long>(hr), internet);
  Check(restricted >= -1 && restricted <= 1, "1, 0 or unknown");
  Check((restricted >= 0) == (hr == S_OK), "an answer exactly when Windows answered S_OK");
  if (internet == 1) Check(restricted >= 0, "with an internet connection, Windows answers");
  HRESULT hr2 = S_OK;
  Check(gesher::net::BackgroundDataRestricted(&hr2) == restricted, "the same answer twice");
  Check(gesher::net::BackgroundDataRestricted(nullptr) == restricted, "no hr pointer");
  std::printf("%s: %d checks, %d failures\n",
              g_failures == 0 ? "background-data PASSED" : "background-data FAILED", g_checks, g_failures);
  CoUninitialize();
  return g_failures == 0 ? 0 : 1;
}
