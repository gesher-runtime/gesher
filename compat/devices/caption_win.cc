// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "caption_win.h"

#include <atomic>
#include <mutex>

#include "winrt.h"

namespace gesher::devices::captions {
namespace {

namespace winrt = gesher::devices::winrt;

constexpr wchar_t kClass[] = L"Windows.Media.ClosedCaptioning.ClosedCaptionProperties";

// Windows.UI.Color: A, R, G, B bytes.
struct Color {
  BYTE a, r, g, b;
};

// windows.media.closedcaptioning.idl (Windows SDK; mingw-w64 has the same), in vtable order.
struct IClosedCaptionPropertiesStatics : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_FontColor(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ComputedFontColor(Color* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontOpacity(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontSize(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontStyle(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_FontEffect(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ComputedBackgroundColor(Color* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BackgroundOpacity(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RegionColor(int32_t* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ComputedRegionColor(Color* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_RegionOpacity(int32_t* value) = 0;
};

// IClosedCaptionPropertiesStatics2 (windows-rs, Windows SDK 10.0.22621+).
struct IClosedCaptionPropertiesStatics2 : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE add_PropertiesChanged(IUnknown* handler, int64_t* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_PropertiesChanged(int64_t token) = 0;
};

// Windows.Foundation.EventHandler<IInspectable>.
struct IInspectableEventHandler : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Invoke(winrt::IInspectable* sender, winrt::IInspectable* args) = 0;
};

const IID IID_IClosedCaptionPropertiesStatics = {
    0x10aa1f84, 0xcc30, 0x4141, {0xb5, 0x03, 0x52, 0x72, 0x28, 0x9e, 0x0c, 0x20}};
const IID IID_IClosedCaptionPropertiesStatics2 = {
    0x9de26870, 0x37de, 0x4197, {0x88, 0x45, 0x9a, 0x48, 0xdc, 0x5a, 0xc3, 0x17}};
// pinterface({9de1c535-6ae1-11e0-84e1-18a905bcc53f};cinterface(IInspectable))
const IID IID_EventHandlerOfInspectable = {
    0xc50898f6, 0xc536, 0x5f47, {0x85, 0x83, 0x8b, 0x2c, 0x24, 0x38, 0xa1, 0x3b}};
// IAgileObject {94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90}: the handler may run on any thread.
const IID IID_IAgileObjectLocal = {
    0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

uint32_t Argb(const Color& c) {
  return (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.r) << 16) |
         (static_cast<uint32_t>(c.g) << 8) | c.b;
}

// The watch: a counter of Windows' notifications and an event for AwaitChange.
std::mutex g_mutex;
std::atomic<int64_t> g_changes{0};
HANDLE g_changed = nullptr;               // guarded by g_mutex
IClosedCaptionPropertiesStatics2* g_statics2 = nullptr;  // guarded by g_mutex
int64_t g_token = 0;                      // guarded by g_mutex

class Handler final : public IInspectableEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_EventHandlerOfInspectable) ||
        IsEqualIID(iid, IID_IAgileObjectLocal)) {
      *out = static_cast<IInspectableEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE Invoke(winrt::IInspectable*, winrt::IInspectable*) override {
    // A Windows thread: only count and wake; never Java.
    g_changes.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_changed != nullptr) SetEvent(g_changed);
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
};

}  // namespace

HRESULT Read(Properties* out) {
  if (out == nullptr) return E_POINTER;
  HRESULT hr = winrt::EnsureApartment();
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;
  IClosedCaptionPropertiesStatics* statics = nullptr;
  hr = winrt::GetActivationFactory(kClass, IID_IClosedCaptionPropertiesStatics,
                                   reinterpret_cast<void**>(&statics));
  if (FAILED(hr)) return hr;
  Properties p = {};
  Color font = {}, background = {}, region = {};
  if (SUCCEEDED(hr)) hr = statics->get_FontColor(&p.font_color);
  if (SUCCEEDED(hr)) hr = statics->get_FontOpacity(&p.font_opacity);
  // Default color + opacity leaves the choice to the player. Windows can return
  // E_INVALIDARG for its computed color in that state; CaptioningSettings ignores
  // that field anyway. Read it whenever either preference is specified, so a
  // failure cannot silently invent the missing RGB or alpha of a partial choice.
  if (SUCCEEDED(hr) && (p.font_color != 0 || p.font_opacity != 0)) {
    hr = statics->get_ComputedFontColor(&font);
  }
  if (SUCCEEDED(hr)) hr = statics->get_FontSize(&p.font_size);
  if (SUCCEEDED(hr)) hr = statics->get_FontStyle(&p.font_style);
  if (SUCCEEDED(hr)) hr = statics->get_FontEffect(&p.font_effect);
  if (SUCCEEDED(hr)) hr = statics->get_BackgroundColor(&p.background_color);
  if (SUCCEEDED(hr)) hr = statics->get_BackgroundOpacity(&p.background_opacity);
  if (SUCCEEDED(hr) && (p.background_color != 0 || p.background_opacity != 0)) {
    hr = statics->get_ComputedBackgroundColor(&background);
  }
  if (SUCCEEDED(hr)) hr = statics->get_RegionColor(&p.region_color);
  if (SUCCEEDED(hr)) hr = statics->get_RegionOpacity(&p.region_opacity);
  if (SUCCEEDED(hr) && (p.region_color != 0 || p.region_opacity != 0)) {
    hr = statics->get_ComputedRegionColor(&region);
  }
  statics->Release();
  if (FAILED(hr)) return hr;
  p.computed_font_color = Argb(font);
  p.computed_background_color = Argb(background);
  p.computed_region_color = Argb(region);
  *out = p;
  return S_OK;
}

HRESULT StartWatch() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_statics2 != nullptr) return S_FALSE;
  HRESULT hr = winrt::EnsureApartment();
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;
  IClosedCaptionPropertiesStatics2* statics2 = nullptr;
  hr = winrt::GetActivationFactory(kClass, IID_IClosedCaptionPropertiesStatics2,
                                   reinterpret_cast<void**>(&statics2));
  if (FAILED(hr)) return hr;
  HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (changed == nullptr) {
    statics2->Release();
    return HRESULT_FROM_WIN32(GetLastError());
  }
  Handler* handler = new Handler();
  int64_t token = 0;
  hr = statics2->add_PropertiesChanged(handler, &token);
  handler->Release();  // Windows holds its own reference while registered
  if (FAILED(hr)) {
    CloseHandle(changed);
    statics2->Release();
    return hr;
  }
  g_changed = changed;
  g_statics2 = statics2;
  g_token = token;
  return S_OK;
}

int64_t AwaitChange(int64_t seen, uint32_t timeout_ms) {
  HANDLE changed;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_changed == nullptr) return -1;
    changed = g_changed;
  }
  int64_t now = g_changes.load();
  if (now != seen) return now;
  WaitForSingleObject(changed, timeout_ms);
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_changed == nullptr ? -1 : g_changes.load();
}

void StopWatch() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_statics2 == nullptr) return;
  g_statics2->remove_PropertiesChanged(g_token);
  g_statics2->Release();
  g_statics2 = nullptr;
  // Wake a waiter; it sees no watch and returns -1. The handle is not closed while a
  // waiter may still hold it: the event object lives for the process (one per watch).
  SetEvent(g_changed);
  g_changed = nullptr;
}

}  // namespace gesher::devices::captions
