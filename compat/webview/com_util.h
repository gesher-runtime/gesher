// Gesher: small COM helpers for the WebView2 host (no WRL in llvm-mingw's
// headers that WebView2's samples rely on): a reference-holding pointer and
// callback objects for WebView2's handler interfaces.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>

#include "webview2_sdk.h"

namespace gesher::webview {

// Holds one reference to a COM object.
template <typename T>
class Com {
 public:
  Com() = default;
  Com(std::nullptr_t) {}
  explicit Com(T* p, bool add_ref = true) : p_(p) {
    if (p_ != nullptr && add_ref) p_->AddRef();
  }
  Com(const Com& o) : p_(o.p_) {
    if (p_ != nullptr) p_->AddRef();
  }
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ~Com() { Reset(); }
  Com& operator=(const Com& o) {
    if (this != &o) {
      if (o.p_ != nullptr) o.p_->AddRef();
      Reset();
      p_ = o.p_;
    }
    return *this;
  }
  Com& operator=(Com&& o) noexcept {
    if (this != &o) {
      Reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  void Reset() {
    if (p_ != nullptr) {
      T* p = p_;
      p_ = nullptr;
      p->Release();
    }
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  // For out parameters: releases what is held first.
  T** Put() {
    Reset();
    return &p_;
  }
  // QueryInterface into another holder; false when the object lacks it.
  template <typename U>
  bool As(REFIID iid, Com<U>* out) const {
    if (p_ == nullptr) return false;
    return SUCCEEDED(p_->QueryInterface(iid, reinterpret_cast<void**>(out->Put()))) &&
           out->get() != nullptr;
  }

 private:
  T* p_ = nullptr;
};

// A string WebView2 allocated (CoTaskMemAlloc).
class CoString {
 public:
  CoString() = default;
  CoString(const CoString&) = delete;
  CoString& operator=(const CoString&) = delete;
  ~CoString() { CoTaskMemFree(s_); }
  LPWSTR* Put() {
    CoTaskMemFree(s_);
    s_ = nullptr;
    return &s_;
  }
  std::wstring str() const { return s_ != nullptr ? std::wstring(s_) : std::wstring(); }

 private:
  LPWSTR s_ = nullptr;
};

// A handler object for one of WebView2's callback interfaces I (whose only
// method is Invoke), calling fn with Invoke's arguments.
template <typename I, typename M>
class Handler;

template <typename I, typename C, typename... A>
class Handler<I, HRESULT (STDMETHODCALLTYPE C::*)(A...)> final : public I {
 public:
  Handler(REFIID iid, std::function<HRESULT(A...)> fn) : iid_(iid), fn_(std::move(fn)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
      *out = static_cast<I*>(this);
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
  HRESULT STDMETHODCALLTYPE Invoke(A... args) override { return fn_ ? fn_(args...) : S_OK; }

 private:
  ~Handler() = default;
  IID iid_;
  std::atomic<ULONG> refs_{1};
  std::function<HRESULT(A...)> fn_;
};

// Callback<ICoreWebView2FooHandler>(IID_ICoreWebView2FooHandler, lambda): a new
// handler holding one reference (Com takes it over).
template <typename I, typename F>
Com<I> Callback(REFIID iid, F&& fn) {
  using H = Handler<I, decltype(&I::Invoke)>;
  return Com<I>(new H(iid, std::forward<F>(fn)), /*add_ref=*/false);
}

#define GESHER_CALLBACK(I, ...) ::gesher::webview::Callback<I>(IID_##I, __VA_ARGS__)

}  // namespace gesher::webview
