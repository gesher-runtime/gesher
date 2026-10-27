// Own Win32/COM fixture for the production IME adapter, without an AOSP build.
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "ime.h"
#include "text_service.h"

// Observe deletion of one native owner without retaining it or adding a
// production lifetime callback. All allocations keep the normal malloc/free
// pairing in this standalone native fixture.
namespace {
std::atomic<void*> observed_owner{nullptr};
std::atomic<int> owner_deletions{0};
}
void* operator new(std::size_t size) {
  if (void* memory = std::malloc(size != 0 ? size : 1)) return memory;
  std::abort(); // The production llvm-mingw build disables C++ exceptions.
}
void operator delete(void* memory) noexcept {
  if (memory != nullptr && memory == observed_owner.load()) ++owner_deletions;
  std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }

// This fixture exercises TSF, not the key map or the touch keyboard service.
namespace gesher::input {
bool KeyEventTypes(int32_t, int32_t, int32_t, uint32_t) { return false; }
int32_t ShowTouchKeyboard(void*, bool) { return 0; }
// Friend inspection does not retain the wrapper whose ownership is tested.
struct TextServiceProbeAccess {
  static void* Owner(ImeWindow* window) { return window->impl_; }
  static TextStore* Store(TextService* service) { return service->store_; }
  static ITextStoreACPSink* Sink(TextStore* store) { return store->sink_; }
  static bool Focused(TextService* service) {
    ITfDocumentMgr* document = nullptr;
    HRESULT result = service->manager_->GetFocus(&document);
    bool same = SUCCEEDED(result) && document == service->document_;
    if (document != nullptr) document->Release();
    return same;
  }
  static bool Associated(TextService* service) {
    ITfDocumentMgr* previous = nullptr;
    HRESULT result = service->manager_->AssociateFocus(service->window_, service->document_, &previous);
    bool same = SUCCEEDED(result) && previous == service->document_;
    if (previous != nullptr) previous->Release();
    return same;
  }
};
}
using namespace gesher::input;
namespace {
int failures = 0;
bool detach_on_text = false;
std::unique_ptr<ImeWindow> ime;
class ExpectedDisconnectScope {
 public:
  explicit ExpectedDisconnectScope(const wchar_t* path) {
    module_ = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_ != nullptr) set_ = reinterpret_cast<Set>(reinterpret_cast<void*>(
        GetProcAddress(module_, "gesher_test_expect_disconnect")));
    if (set_ != nullptr) set_(TRUE);
  }
  ~ExpectedDisconnectScope() {
    if (set_ != nullptr) set_(FALSE);
    if (module_ != nullptr) FreeLibrary(module_);
  }
  bool Available() const { return set_ != nullptr; }
 private:
  using Set = void(WINAPI*)(BOOL);
  HMODULE module_ = nullptr;
  Set set_ = nullptr;
};
class ChangeSink final : public ITextStoreACPSink {
 public:
  std::weak_ptr<TextService> service;
  std::function<void()> change;
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
    called = true;
    if (change) change(); else ime.reset();
    retained = !service.expired(); return S_OK;
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
    replaced |= edit.kind == TextEvent::kDocumentCompositionText && edit.text == u"\u65e5\u672c"
        && edit.start == 7 && edit.end == 11;
    ended |= edit.kind == TextEvent::kDocumentCompositionEnd && edit.text == u"\u65e5\u672c";
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
  {
    ExpectedDisconnectScope expected(path.c_str());
    Check(expected.Available(), "the native negative service fixture declares its expected disconnect");
    detach_on_text = true;
    result = ime->RunTextServiceTest();
  }
  Check(FAILED(static_cast<HRESULT>(result)) && ime == nullptr,
      "a nested native owner detach explicitly interrupts its real COM edit session with a failed HRESULT");
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
    auto* change = new ChangeSink(); change->service = weak;
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
  SetFocus(window);
  Check(GetFocus() == window, "the replacement fixture has actual native HWND focus");
  Editor(window, 5);
  weak = ime->TextServiceForTest();
  inspect = weak.lock();
  if (inspect != nullptr) {
    TextStore* store = TextServiceProbeAccess::Store(inspect.get());
    store->AddRef();
    ITextStoreACPSink* original = TextServiceProbeAccess::Sink(store);
    if (original != nullptr) original->AddRef();
    if (original != nullptr) store->UnadviseSink(original);
    auto* change = new ChangeSink(); change->service = weak;
    change->change = [&] {
      TextInputState next; next.active = next.ime = true; next.editor_epoch = 6;
      ime->SetTextInput(next);
      TextDocument doc; doc.text = u"before \u05e9\u05dc\u05d5\u05dd";
      doc.start = 7; doc.end = 11; doc.epoch = 6; doc.available = true;
      ime->SetDocument(doc); Drain();
    };
    HRESULT advised = store->AdviseSink(IID_ITextStoreACPSink, change, TS_AS_ALL_SINKS);
    inspect.reset();
    TextDocument update; update.text = u"replace this editor during its notification";
    update.epoch = 5; update.start = update.end = static_cast<int32_t>(update.text.size());
    update.available = true;
    ime->SetDocument(update); Drain();
    auto current = ime->TextServiceForTest().lock();
    bool focused = current != nullptr && TextServiceProbeAccess::Focused(current.get());
    bool associated = current != nullptr && TextServiceProbeAccess::Associated(current.get());
    if (!focused || !associated) std::printf("INFO windows4 IME TSF replacement: focused %d associated %d\n", focused, associated);
    Check(SUCCEEDED(advised) && change->called && change->retained && weak.expired()
            && focused && associated,
        "an old reentrant TSF owner cannot replace the new editor's focus or HWND association when its lease returns");
    bool next_document = ime->RunTextServiceTest() == S_OK;
    bool next_edit = false;
    while (ime->TakeTextEvent(&event))
      next_edit |= event.epoch == 6 && event.kind == TextEvent::kDocumentCompositionText
          && event.text == u"\u65e5\u672c" && event.start == 7 && event.end == 11;
    Check(next_document && next_edit,
        "nested editor publications consumed during a TSF handoff are replayed into the latest real document");
    store->UnadviseSink(change);
    if (original != nullptr) original->Release();
    store->Release(); change->Release();
  } else { Check(false, "the replacement fixture obtains its window-owned TSF context"); }
  ime.reset();
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
  ime.reset(); Drain();
  Editor(window, 20);
  ime->SetCaret(12, 34, 2, 20, 20); Drain();
  TextInputState next_caret; next_caret.active = next_caret.ime = true; next_caret.editor_epoch = 21;
  ime->SetTextInput(next_caret);
  ime->SetCaret(56, 78, 2, 20, 21);
  ime->SetCaret(1, 2, 2, 20, 20); Drain();
  POINT position{};
  Check(GetCaretPos(&position) && position.x == 56 && position.y == 78,
      "a delayed old-editor caret cannot overwrite the newer editor's actual Windows caret");
  ime.reset(); Drain();
  Editor(window, 30);
  TextDocument partial_document; partial_document.available = true; partial_document.epoch = 30;
  partial_document.text = u"abcd"; partial_document.start = 0; partial_document.end = 4;
  ime->SetDocument(partial_document); Drain();
  result = ime->RunTextServiceTest();
  bool partial_text = false, partial_selection = false, partial_end = false;
  while (ime->TakeTextEvent(&event)) {
    partial_text |= event.epoch == 30 && event.kind == TextEvent::kDocumentCompositionText
        && event.text == u"aXcd" && event.start == 0 && event.end == 4;
    partial_selection |= event.epoch == 30 && event.kind == TextEvent::kDocumentSelect
        && event.start == 2 && event.end == 2;
    partial_end |= event.epoch == 30 && event.kind == TextEvent::kDocumentCompositionEnd && event.text == u"aXcd";
  }
  Check(result == S_OK && partial_text && partial_selection && partial_end,
      "an actual Windows text-service partial composition preserves full text, insertion caret and completion through the production IME bridge");
  ime.reset(); Drain();
  Editor(window, 12);
  observed_owner.store(TextServiceProbeAccess::Owner(ime.get()));
  owner_deletions.store(0);
  DestroyWindow(window); Drain();
  ime.reset();
  Check(owner_deletions.load() == 1,
      "DestroyWindow before connection release frees the native owner exactly once after WM_NCDESTROY");
  observed_owner.store(nullptr);
  CoUninitialize();
  std::printf("%d windows4 IME TSF check(s) failed\n", failures);
  return failures;
}
