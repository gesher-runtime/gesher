// A COM source calls the production IFrameworkInputPane handler. Window events
// are read from the real Gesher HWND; no kMsgInputPane injection is used.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include "gesher_window.h"
#include "input_pane.h"

namespace {
int failures = 0;
std::atomic<int> live_sources{0};
bool initial_location = false;
bool fail_advise = false;
DWORD source_cookie = 77;

void Check(bool okay, const char* label) {
  std::printf("%s  windows4 COM input pane: %s\n", okay ? "PASS" : "FAIL", label);
  std::fflush(stdout);
  if (!okay) ++failures;
}

class Source final : public gesher::FrameworkInputPane {
 public:
  Source() : initial_(initial_location), fail_(fail_advise), cookie_(source_cookie) { ++live_sources; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, gesher::kInputPaneIID)) {
      *out = static_cast<gesher::FrameworkInputPane*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG count = InterlockedDecrement(&references_);
    if (count == 0) { --live_sources; delete this; }
    return static_cast<ULONG>(count);
  }
  HRESULT STDMETHODCALLTYPE Advise(IUnknown*, gesher::FrameworkInputPaneHandler*, DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AdviseWithHWND(HWND hwnd, gesher::FrameworkInputPaneHandler* handler,
                                         DWORD* cookie) override {
    hwnd_ = hwnd;
    if (fail_) { ready.store(true); return E_FAIL; }
    if (cookie == nullptr || handler == nullptr) return E_POINTER;
    void* typed = nullptr;
    HRESULT result = handler->QueryInterface(gesher::kInputPaneHandlerIID, &typed);
    if (FAILED(result)) return result;
    handler_ = static_cast<gesher::FrameworkInputPaneHandler*>(typed);
    void* identity = nullptr;
    identity_ok = SUCCEEDED(handler_->QueryInterface(IID_IUnknown, &identity)) && identity == typed;
    if (identity != nullptr) static_cast<IUnknown*>(identity)->Release();
    *cookie = cookie_;
    ready.store(true);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
    if (cookie != cookie_ || handler_ == nullptr) return E_INVALIDARG;
    handler_->Release(); handler_ = nullptr; ++unadvised;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Location(RECT* rectangle) override {
    if (rectangle == nullptr) return E_POINTER;
    *rectangle = RECT{};
    if (initial_) {
      RECT client; POINT origin = {};
      GetClientRect(hwnd_, &client); ClientToScreen(hwnd_, &origin);
      *rectangle = RECT{origin.x, origin.y + 620, origin.x + client.right, origin.y + client.bottom};
    }
    return S_OK;
  }
  HRESULT Show(RECT rectangle) { return handler_ == nullptr ? E_UNEXPECTED : handler_->Showing(&rectangle, TRUE); }
  HRESULT Hide() { return handler_ == nullptr ? E_UNEXPECTED : handler_->Hiding(TRUE); }
  std::atomic<bool> ready{false};
  bool identity_ok = false;
  int unadvised = 0;
 private:
  LONG references_ = 1;
  const bool initial_, fail_;
  const DWORD cookie_;
  HWND hwnd_ = nullptr;
  gesher::FrameworkInputPaneHandler* handler_ = nullptr;
};

std::atomic<Source*> current_source{nullptr};
void* Factory() {
  auto* source = new Source();
  source->AddRef(); // The probe owns one reference after the subscription goes.
  current_source.store(source);
  return static_cast<gesher::FrameworkInputPane*>(source);
}
Source* WaitSource() {
  ULONGLONG until = GetTickCount64() + 3000;
  while (GetTickCount64() < until) {
    Source* source = current_source.load();
    if (source != nullptr && source->ready.load()) return source;
    Sleep(10);
  }
  return nullptr;
}
bool Event(gesher::Window* window, int top, int bottom) {
  ULONGLONG until = GetTickCount64() + 3000;
  gesher::WindowEvent event;
  while (GetTickCount64() < until) {
    if (window->WaitEvent(&event, 100) && event.type == gesher::WindowEvent::kInputPane &&
        event.button == top && event.modifiers == bottom) return true;
  }
  return false;
}
}

int main() {
  auto window = std::unique_ptr<gesher::Window>(gesher::Window::Create("Gesher input pane COM fixture", 400, 800, Factory));
  Source* source = WaitSource();
  Check(window != nullptr && source != nullptr && source->identity_ok,
        "AdviseWithHWND obtains the real handler through QueryInterface and stable IUnknown identity");
  if (window == nullptr || source == nullptr) return 1;
  window->ShowOffscreen();
  int bounds[4]; window->ScreenBounds(bounds);
  RECT rectangle = {bounds[0], bounds[1] + 500, bounds[2], bounds[3]};
  Check(SUCCEEDED(source->Show(rectangle)) && Event(window.get(), 500, 800),
        "Showing callback reaches a real window's kInputPane event with client rows 500-800");
  rectangle.left = bounds[2] + 10; rectangle.right = bounds[2] + 100;
  Check(SUCCEEDED(source->Show(rectangle)) && Event(window.get(), -1, 0),
        "a pane horizontally outside the window does not cover its rows");
  rectangle = RECT{bounds[0], bounds[1] + 600, bounds[2], bounds[3]};
  Check(SUCCEEDED(source->Show(rectangle)) && Event(window.get(), 600, 800),
        "changed callback geometry reaches the same window");
  Check(SUCCEEDED(source->Hide()) && Event(window.get(), -1, 0), "Hiding callback clears IME occlusion");
  window.reset();
  Check(source->unadvised == 1, "window destruction Unadvises the exact cookie and releases its handler");
  current_source.store(nullptr); source->Release();
  Check(live_sources.load() == 0, "the COM source has no references after window and probe release");

  initial_location = true; source_cookie = 0; // Zero is still an opaque valid cookie.
  window.reset(gesher::Window::Create("Gesher initial pane COM fixture", 400, 800, Factory));
  source = WaitSource();
  Check(window != nullptr && source != nullptr && Event(window.get(), 620, 800),
        "Location reports an already visible pane before the first callback");
  window.reset();
  Check(source != nullptr && source->unadvised == 1, "an opaque zero cookie is also Unadvised");
  if (source != nullptr) { current_source.store(nullptr); source->Release(); }

  initial_location = false; fail_advise = true;
  window.reset(gesher::Window::Create("Gesher failed pane COM fixture", 400, 800, Factory));
  source = WaitSource();
  window.reset();
  Check(source != nullptr && source->unadvised == 0, "a failed Advise is not Unadvised");
  if (source != nullptr) { current_source.store(nullptr); source->Release(); }
  Check(live_sources.load() == 0, "failed Advise also releases the source and handler");
  std::printf("%d windows4 COM input pane check(s) failed\n", failures);
  return failures;
}
