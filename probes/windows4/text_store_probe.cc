// Native COM contract fixture, independent of ART. The TSF manager/edit-service
// and InputConnection integration are separate from these ACP invariants.
#include <windows.h>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "text_store.h"

namespace {
using gesher::input::DocumentEdit;
using gesher::input::TextDocument;
using gesher::input::TextStore;
int failures = 0;
void Check(bool okay, const char* label) {
  std::printf("%s  windows4 TSF store: %s\n", okay ? "PASS" : "FAIL", label);
  std::fflush(stdout);
  if (!okay) ++failures;
}
class Sink final : public ITextStoreACPSink {
 public:
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
  HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override { ++text; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSelectionChange() override { ++selection; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override { ++layout; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { ++status; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD flags) override { ++locks; return action ? action(flags) : S_OK; }
  HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
  HRESULT Lock(TextStore* store, DWORD flags = TS_LF_SYNC | TS_LF_READWRITE) {
    HRESULT session = E_FAIL;
    requesting = true;
    HRESULT result = store->RequestLock(flags, &session);
    requesting = false;
    return FAILED(result) ? result : session;
  }
  void Clear() { text = selection = layout = status = nested_notifications = 0; }
  std::function<HRESULT(DWORD)> action;
  int text = 0, selection = 0, layout = 0, status = 0, locks = 0, nested_notifications = 0;
  bool requesting = false;
 private:
  LONG references_ = 1;
};
std::u16string Text(TextStore* store, Sink* sink) {
  std::u16string result;
  sink->action = [&](DWORD) -> HRESULT {
    WCHAR buffer[256]; ULONG count = 0, runs = 0; LONG next = 0; TS_RUNINFO run;
    HRESULT status = store->GetText(0, -1, buffer, 256, &count, &run, 1, &runs, &next);
    if (SUCCEEDED(status)) result.assign(buffer, buffer + count);
    return status;
  };
  if (FAILED(sink->Lock(store, TS_LF_SYNC | TS_LF_READ))) ++failures;
  sink->action = nullptr;
  return result;
}
}

#include "tsf_service_fixture.h"

int main() {
  HWND window = CreateWindowExW(0, L"STATIC", L"Gesher TSF ACP fixture", WS_OVERLAPPED,
                               0, 0, 400, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window) return 1;
  std::vector<DocumentEdit> edits;
  int scheduled = 0;
  auto* store = new TextStore(window, [&](DocumentEdit edit) { edits.push_back(std::move(edit)); }, [&] { ++scheduled; });
  auto* sink = new Sink();
  Check(SUCCEEDED(store->AdviseSink(IID_ITextStoreACPSink, sink, TS_AS_ALL_SINKS)), "real ITextStoreACPSink subscription");
  void* identity = nullptr; void* composition = nullptr; void* identity2 = nullptr;
  store->QueryInterface(IID_IUnknown, &identity);
  store->QueryInterface(IID_ITfContextOwnerCompositionSink, &composition);
  static_cast<ITfContextOwnerCompositionSink*>(composition)->QueryInterface(IID_IUnknown, &identity2);
  Check(identity == identity2, "ACP and composition interfaces share canonical IUnknown identity");
  static_cast<IUnknown*>(identity)->Release(); static_cast<IUnknown*>(identity2)->Release();
  static_cast<ITfContextOwnerCompositionSink*>(composition)->Release();

  TextDocument original; original.available = true; original.epoch = 1;
  original.text = u"before \u05e9\u05dc\u05d5\u05dd"; original.start = 7; original.end = 11;
  store->Update(original);
  LONG end = 0;
  Check(store->GetEndACP(&end) == TS_E_NOLOCK && Text(store, sink) == original.text,
        "UTF-16 document access requires a granted read lock");
  sink->action = [&](DWORD) -> HRESULT {
    TS_SELECTION_ACP selection; ULONG fetched = 0;
    HRESULT result = store->GetSelection(TS_DEFAULT_SELECTION, 1, &selection, &fetched);
    Check(SUCCEEDED(result) && fetched == 1 && selection.acpStart == 7 && selection.acpEnd == 11,
          "selection uses UTF-16 ACP positions");
    WCHAR buffer[2]; ULONG copied = 0, returned = 0; LONG next = 0; TS_RUNINFO run;
    result = store->GetText(7, -1, buffer, 2, &copied, &run, 1, &returned, &next);
    Check(SUCCEEDED(result) && copied == 2 && returned == 1 && run.uCount == 2 && next == 9,
          "bounded GetText returns a matching run and next ACP");
    TS_TEXTCHANGE change;
    Check(store->SetText(0, 7, 11, L"x", 1, &change) == TS_E_NOLOCK,
          "a read lock cannot mutate the document");
    HRESULT upgrade = E_FAIL;
    result = store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &upgrade);
    Check(SUCCEEDED(result) && upgrade == TS_E_SYNCHRONOUS, "nested synchronous write locks are refused");
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ); sink->action = nullptr;
  sink->Clear();
  sink->action = [&](DWORD flags) -> HRESULT {
    if ((flags & TS_LF_READWRITE) == TS_LF_READWRITE) {
      TS_TEXTCHANGE change;
      HRESULT result = store->SetText(0, 7, 11, L"\u65e5\u672c", 2, &change);
      Check(SUCCEEDED(result) && change.acpStart == 7 && change.acpOldEnd == 11 && change.acpNewEnd == 9,
            "a real write grant replaces the selected document range");
      return result;
    }
    HRESULT session = E_FAIL;
    HRESULT result = store->RequestLock(TS_LF_READWRITE, &session);
    Check(SUCCEEDED(result) && session == TS_S_ASYNC, "read-to-write upgrade is asynchronous");
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ);
  Check(edits.empty() && scheduled > 0, "the upgrade does not run inside the original RequestLock");
  store->Pump(); sink->action = nullptr;
  Check(edits.size() == 1 && edits[0].text == u"\u65e5\u672c" && edits[0].start == 7 && edits[0].end == 11
          && edits[0].epoch == 1 && edits[0].revision == 1 && sink->text == 0 && sink->selection == 0,
        "TSF edits carry exact replacement/revision and do not echo change notifications");
  Check(Text(store, sink) == u"before \u65e5\u672c", "subsequent TSF reads observe the accepted edit");
  store->Update(original);
  Check(Text(store, sink) == u"before \u65e5\u672c", "an older asynchronous InputConnection reply cannot erase TSF edits");

  TextDocument accepted = original; accepted.text = u"before \u65e5\u672c"; accepted.start = accepted.end = 9; accepted.applied_revision = 1;
  store->Update(accepted);
  sink->Clear();
  sink->action = [&](DWORD) -> HRESULT {
    TextDocument next = accepted; next.text = u"external"; next.start = next.end = 8;
    store->Update(next);
    RECT caret = {10, 20, 11, 42}; store->Caret(caret);
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ); sink->action = nullptr;
  Check(sink->text == 0 && sink->layout == 0, "application document/layout changes are deferred outside RequestLock");
  store->Pump();
  Check(sink->text == 1 && sink->selection == 1 && sink->layout == 1 && sink->nested_notifications == 0,
        "deferred application text, selection and layout notifications are delivered once");
  TextDocument restricted; restricted.epoch = 2; restricted.text = u"must not leak"; restricted.available = false;
  store->Update(restricted);
  TS_STATUS status; store->GetStatus(&status);
  Check((status.dwDynamicFlags & TS_SD_READONLY) != 0 && Text(store, sink).empty(),
        "a restricted editor is read-only and retains no document text");
  store->Update(accepted);
  Check(Text(store, sink).empty(), "a reply from an old focused editor cannot expose its document");
  Check(SUCCEEDED(store->UnadviseSink(sink)), "the actual sink can unsubscribe by canonical COM identity");
  store->Release(); sink->Release();
  TestWindowsTextService(window);
  DestroyWindow(window);
  std::printf("%d windows4 TSF store check(s) failed\n", failures);
  return failures;
}
