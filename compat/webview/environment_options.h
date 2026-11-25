// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The exact host options are also used by same-profile production fixtures.
#pragma once

#include "com_util.h"
#include <cstring>

namespace gesher::webview {

// ICoreWebView2EnvironmentOptions, as WebView2EnvironmentOptions.h would
// give with WRL.
class EnvironmentOptions final : public ICoreWebView2EnvironmentOptions {
 public:
  explicit EnvironmentOptions(std::wstring args) : args_(std::move(args)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions)) {
      *out = static_cast<ICoreWebView2EnvironmentOptions*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
    return Copy(args_, value);
  }
  HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
    args_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override { return Copy(language_, value); }
  HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
    language_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
    return Copy(target_, value);
  }
  HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
    target_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
    if (allow == nullptr) return E_POINTER;
    *allow = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override {
    return S_OK;
  }

 private:
  ~EnvironmentOptions() = default;
  static HRESULT Copy(const std::wstring& s, LPWSTR* out) {
    if (out == nullptr) return E_POINTER;
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (*out == nullptr) return E_OUTOFMEMORY;
    memcpy(*out, s.c_str(), bytes);
    return S_OK;
  }
  std::atomic<ULONG> refs_{1};
  std::wstring args_;
  std::wstring language_;
  // The oldest runtime Gesher's host works with: newer interfaces are asked
  // for with QueryInterface and used when the runtime has them.
  std::wstring target_ = L"86.0.616.0";
};

// Chromium otherwise stops drawing windows it considers occluded/offscreen.
// Keep the host and any same-profile observer environment byte-compatible.
inline std::wstring HostBrowserArguments(const std::wstring& extra) {
  std::wstring all = L"--disable-features=CalculateNativeWinOcclusion "
                     L"--disable-backgrounding-occluded-windows";
  if (!extra.empty()) all += L" " + extra;
  return all;
}

}  // namespace gesher::webview
