#include "input_pane.h"
#include <objbase.h>
#include <mutex>
#include "gesher_window.h"

namespace gesher {
namespace {
const CLSID kInputPaneClass = {0xD5120AA3, 0x46BA, 0x44C5,
                               {0x82, 0x2D, 0xCA, 0x80, 0x92, 0xC1, 0xFC, 0x72}};
}
const IID kInputPaneIID = {0x5752238B, 0x24F0, 0x495A,
                           {0x82, 0xF1, 0x2F, 0xD5, 0x93, 0x05, 0x67, 0x96}};
const IID kInputPaneHandlerIID = {0x226C537B, 0x1E76, 0x4D9E,
                                  {0xA7, 0x60, 0x33, 0xDB, 0x29, 0x92, 0x2F, 0x18}};

class InputPane::Handler final : public FrameworkInputPaneHandler {
 public:
  explicit Handler(HWND hwnd) : hwnd_(hwnd) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, kInputPaneHandlerIID)) {
      *out = static_cast<FrameworkInputPaneHandler*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG count = InterlockedDecrement(&references_);
    if (count == 0) delete this;
    return static_cast<ULONG>(count);
  }
  HRESULT STDMETHODCALLTYPE Showing(RECT* screen, BOOL) override {
    if (screen == nullptr) return E_POINTER;
    Set(*screen);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Hiding(BOOL) override { Set(RECT{}); return S_OK; }
  void Screen(int out[4]) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out[0] = rectangle_.left; out[1] = rectangle_.top;
    out[2] = rectangle_.right; out[3] = rectangle_.bottom;
  }
 private:
  void Set(const RECT& rectangle) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      rectangle_ = rectangle;
    }
    // No pointers cross the message queue. Keep both horizontal and vertical
    // extents: a floating pane outside this window must not obscure its rows.
    PostMessageW(hwnd_, kMsgInputPaneChanged, 0, 0);
  }
  HWND hwnd_;
  LONG references_ = 1;
  mutable std::mutex mutex_;
  RECT rectangle_ = {};
};

std::unique_ptr<InputPane> InputPane::Attach(void* hwnd, Factory factory) {
  auto subscription = std::unique_ptr<InputPane>(new InputPane());
  if (factory != nullptr) {
    subscription->source_ = static_cast<FrameworkInputPane*>(factory());
  } else {
    HRESULT result = CoCreateInstance(kInputPaneClass, nullptr, CLSCTX_INPROC_SERVER,
        kInputPaneIID, reinterpret_cast<void**>(&subscription->source_));
    if (FAILED(result)) return nullptr;
  }
  if (subscription->source_ == nullptr) return nullptr;
  subscription->handler_ = new Handler(static_cast<HWND>(hwnd));
  HRESULT result = subscription->source_->AdviseWithHWND(static_cast<HWND>(hwnd),
      subscription->handler_, &subscription->cookie_);
  if (FAILED(result)) {
    return nullptr;
  }
  subscription->advised_ = true;
  RECT rectangle = {};
  if (SUCCEEDED(subscription->source_->Location(&rectangle)) &&
      rectangle.right > rectangle.left && rectangle.bottom > rectangle.top) {
    subscription->handler_->Showing(&rectangle, FALSE);
  }
  return subscription;
}

InputPane::~InputPane() {
  if (source_ != nullptr) {
    if (advised_) source_->Unadvise(cookie_);
    source_->Release();
  }
  if (handler_ != nullptr) handler_->Release();
}
void InputPane::Screen(int out[4]) const { handler_->Screen(out); }
}  // namespace gesher
