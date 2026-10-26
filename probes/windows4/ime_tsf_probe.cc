// Own Win32/COM fixture for the production IME adapter, without an AOSP build.
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "ime.h"
#include "text_service.h"

// This fixture exercises TSF, not the key map or the touch keyboard service.
namespace gesher::input {
bool KeyEventTypes(int32_t, int32_t, int32_t, uint32_t) { return false; }
int32_t ShowTouchKeyboard(void*, bool) { return 0; }
// Friend inspection does not retain the wrapper whose ownership is tested.
struct TextServiceProbeAccess {
  static TextStore* Store(TextService* service) { return service->store_; }
  static ITextStoreACPSink* Sink(TextStore* store) { return store->sink_; }
};
}
using namespace gesher::input;
namespace {
int failures = 0;
bool detach_on_text = false;
std::unique_ptr<ImeWindow> ime;
class DetachOnChange final : public ITextStoreACPSink {
 public:
  std::weak_ptr<TextService> service;
  bool called = false, retained = false;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITextStoreACPSink)) {
      *out = static_cast<ITextStoreACPSink*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG count = InterlockedDecrement(&references_);
    if (count == 0) delete this;
    return static_cast<ULONG>(count);
  }
  HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override {
    called = true; ime.reset(); retained = !service.expired(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnSelectionChange() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
 private:
  LONG references_ = 1;
};
void Check(bool okay, const char* what) {
  std::printf("%s windows4 IME TSF: %s\n", okay ? "PASS" : "FAIL", what);
  if (!okay) failures++;
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_CHAR && w == kTextEventMarker) {
    if (detach_on_text) { detach_on_text = false; ime.reset(); }
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
void Drain() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}
void Editor(HWND window, int epoch) {
  ime = ImeWindow::Attach(window);
  TextInputState state; state.active = state.ime = true; state.editor_epoch = epoch;
  ime->SetTextInput(state);
  TextDocument doc; doc.text = u"before \u05e9\u05dc\u05d5\u05dd"; doc.start = 7; doc.end = 11;
  doc.available = true; doc.epoch = epoch;
  ime->SetDocument(doc);
  Drain();
}
}
int main() {
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 1;
  WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpfnWndProc = Procedure;
  type.lpszClassName = L"GesherWindows4ImeTsfProbe";
  RegisterClassW(&type);
  HWND window = CreateWindowW(type.lpszClassName, L"TSF fixture", WS_OVERLAPPEDWINDOW,
      0, 0, 300, 300, nullptr, nullptr, type.hInstance, nullptr);
  if (!window) { CoUninitialize(); return 1; }
  wchar_t module[32768]; DWORD length = GetModuleFileNameW(nullptr, module, 32768);
  std::wstring path(module, module + length);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"windows4_tsf_service.dll";
  SetEnvironmentVariableW(L"GESHER_TSF_TEST_SERVICE", path.c_str());
  Editor(window, 1);
  int32_t result = ime->RunTextServiceTest();
  TextEvent event; std::vector<TextEvent> edits;
  while (ime->TakeTextEvent(&event)) edits.push_back(std::move(event));
  bool replaced = false, ended = false; int revision = 0;
  for (const auto& edit : edits) {
    Check(edit.epoch == 1 && edit.revision > revision, "ordered edits retain the editor epoch and increasing revisions");
    revision = edit.revision;
    replaced |= edit.kind == TextEvent::kDocumentReplace && edit.text == u"\u65e5\u672c"
        && edit.start == 7 && edit.end == 11;
    ended |= edit.kind == TextEvent::kDocumentCompositionEnd;
  }
  Check(result == S_OK && replaced && ended, "the production IME adapter emits exact native reconversion/finish events");
  TextInputState restricted; restricted.active = true; restricted.editor_epoch = 2;
  ime->SetTextInput(restricted);
  // Sent work can run before the posted state update. Shared state must already
  // deny service access, rather than exposing the previous editor's context.
  Check(ime->RunTextServiceTest() == static_cast<int32_t>(E_UNEXPECTED), "a restriction takes effect before its queued window-thread update");
  Drain();
  Check(ime->RunTextServiceTest() == static_cast<int32_t>(E_UNEXPECTED), "a restricted editor destroys its TSF document before service access");
  ime.reset();
  Editor(window, 3);
  detach_on_text = true;
  result = ime->RunTextServiceTest();
  Check(ime == nullptr, "a nested native text callback can detach the owner during a real COM edit session");
  Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == Procedure,
      "reentrant detach restores the original window procedure");
  Editor(window, 4);
  auto weak = ime->TextServiceForTest();
  auto inspect = weak.lock();
  if (inspect != nullptr) {
    TextStore* store = TextServiceProbeAccess::Store(inspect.get());
    store->AddRef(); // Avoid a crash on the old source: assert wrapper lifetime instead.
    ITextStoreACPSink* original = TextServiceProbeAccess::Sink(store);
    if (original != nullptr) original->AddRef();
    if (original != nullptr) store->UnadviseSink(original);
    auto* change = new DetachOnChange(); change->service = weak;
    HRESULT advised = store->AdviseSink(IID_ITextStoreACPSink, change, TS_AS_ALL_SINKS);
    inspect.reset(); // The callback's weak observer must not hide the regression.
    TextDocument update; update.text = u"external document edit"; update.epoch = 4;
    update.start = update.end = static_cast<int32_t>(update.text.size()); update.available = true;
    ime->SetDocument(update); Drain();
    Check(SUCCEEDED(advised) && change->called && change->retained && ime == nullptr && weak.expired(),
        "document Update retains its TSF owner throughout a real OnTextChange COM detach callback, then releases it");
    store->UnadviseSink(change);
    if (original != nullptr) original->Release();
    store->Release(); change->Release();
  } else { Check(false, "the update-detach fixture obtains its window-owned TSF context"); }
  Editor(window, 10);
  TextInputState delayed_finish; delayed_finish.editor_epoch = 9;
  ime->SetTextInput(delayed_finish); Drain();
  Check(ime->RunTextServiceTest() == S_OK,
      "a delayed finish publication cannot disable the newer focused editor");
  TextInputState newest; newest.active = newest.ime = true; newest.editor_epoch = 11;
  ime->SetTextInput(newest);
  TextDocument latest; latest.text = u"before \u05e9\u05dc\u05d5\u05dd";
  latest.start = 7; latest.end = 11; latest.epoch = 11; latest.available = true;
  ime->SetDocument(latest);
  delayed_finish.editor_epoch = 10; ime->SetTextInput(delayed_finish);
  TextDocument stale = latest; stale.text = u"old"; stale.start = stale.end = 0; stale.epoch = 10;
  ime->SetDocument(stale); Drain();
  Check(ime->RunTextServiceTest() == S_OK,
      "late finish and snapshot publications cannot overwrite a newer document waiting on the window queue");
  ime.reset();
  Editor(window, 1);
  Check(ime->RunTextServiceTest() == S_OK,
      "a new native connection starts a fresh editor epoch lifecycle");
  ime.reset(); Drain(); DestroyWindow(window); CoUninitialize();
  std::printf("%d windows4 IME TSF check(s) failed\n", failures);
  return failures;
}
