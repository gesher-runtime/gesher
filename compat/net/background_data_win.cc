// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "background_data_win.h"

#include <objbase.h>
#include <stdint.h>

namespace gesher::net {
namespace {

typedef struct HSTRING__* HSTRING;
struct HSTRING_HEADER {
  void* reserved[5];  // WindowsCreateStringReference's header (24 bytes on x64; padded)
};

// IInspectable's methods after IUnknown's.
struct IInspectable : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int* level) = 0;
};

// windows.networking.connectivity.idl (Windows SDK; mingw-w64 has the same), vtable order.
struct IConnectionCost;
struct IConnectionProfile : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_ProfileName(HSTRING* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNetworkConnectivityLevel(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNetworkNames(IUnknown** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectionCost(IConnectionCost** value) = 0;
};
struct INetworkInformationStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetConnectionProfiles(IUnknown** value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetInternetConnectionProfile(IConnectionProfile** value) = 0;
};
struct IConnectionCost : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_NetworkCostType(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Roaming(boolean* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_OverDataLimit(boolean* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ApproachingDataLimit(boolean* value) = 0;
};
// IConnectionCost2 (Windows 8.1; windows-rs): BackgroundDataUsageRestricted only.
struct IConnectionCost2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_BackgroundDataUsageRestricted(boolean* value) = 0;
};

const IID kIidNetworkInformationStatics = {
    0x5074f851, 0x950d, 0x4165, {0x9c, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1e, 0xea}};
const IID kIidConnectionCost2 = {
    0x8e113a05, 0xe209, 0x4549, {0xbb, 0x25, 0x5e, 0x0d, 0xb6, 0x91, 0xcb, 0x05}};

constexpr wchar_t kClass[] = L"Windows.Networking.Connectivity.NetworkInformation";

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using WindowsCreateStringReferenceFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);

template <typename T>
T Combase(const char* name) {
  // combase.dll is found at run time: dalvikvm starts on editions without WinRT.
  HMODULE combase = GetModuleHandleW(L"combase.dll");
  if (combase == nullptr) {
    combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  }
  return combase != nullptr
             ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(combase, name)))
             : nullptr;
}

template <typename T>
void Release(T*& p) {
  if (p != nullptr) {
    p->Release();
    p = nullptr;
  }
}

}  // namespace

int BackgroundDataRestricted(HRESULT* hr_out) {
  HRESULT ignored;
  HRESULT& hr = hr_out != nullptr ? *hr_out : ignored;
  static const auto get_factory = Combase<RoGetActivationFactoryFn>("RoGetActivationFactory");
  static const auto make_string =
      Combase<WindowsCreateStringReferenceFn>("WindowsCreateStringReference");
  if (get_factory == nullptr || make_string == nullptr) {
    hr = E_NOTIMPL;
    return -1;
  }
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
    hr = init;
    return -1;
  }
  int result = -1;
  HSTRING_HEADER header = {};
  HSTRING name = nullptr;
  INetworkInformationStatics* statics = nullptr;
  IConnectionProfile* profile = nullptr;
  IConnectionCost* cost = nullptr;
  IConnectionCost2* cost2 = nullptr;
  hr = make_string(kClass, static_cast<UINT32>(sizeof(kClass) / sizeof(kClass[0]) - 1), &header, &name);
  if (SUCCEEDED(hr)) hr = get_factory(name, kIidNetworkInformationStatics, reinterpret_cast<void**>(&statics));
  if (SUCCEEDED(hr)) hr = statics->GetInternetConnectionProfile(&profile);
  if (SUCCEEDED(hr) && profile == nullptr) hr = S_FALSE;  // no internet connection: unknown
  if (hr == S_OK) hr = profile->GetConnectionCost(&cost);
  if (hr == S_OK) hr = cost->QueryInterface(kIidConnectionCost2, reinterpret_cast<void**>(&cost2));
  if (hr == S_OK) {
    boolean restricted = 0;
    hr = cost2->get_BackgroundDataUsageRestricted(&restricted);
    if (SUCCEEDED(hr)) result = restricted ? 1 : 0;
  }
  Release(cost2);
  Release(cost);
  Release(profile);
  Release(statics);
  if (init == S_OK || init == S_FALSE) CoUninitialize();
  return result;
}

}  // namespace gesher::net
