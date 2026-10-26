// A test text service uses Windows' TSF context and edit-session interfaces to
// read and reconvert an existing selection. It registers no system IME profile.
#pragma once
namespace {
class ReconvertSession final : public ITfEditSession, public ITfCompositionSink {
 public:
  explicit ReconvertSession(ITfContext* context) : context_(context) { context_->AddRef(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfEditSession))
      *out = static_cast<ITfEditSession*>(this);
    else if (IsEqualIID(iid, IID_ITfCompositionSink)) *out = static_cast<ITfCompositionSink*>(this);
    if (*out != nullptr) { AddRef(); return S_OK; }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG count = InterlockedDecrement(&references_);
    if (count == 0) { context_->Release(); delete this; }
    return static_cast<ULONG>(count);
  }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    TF_SELECTION selection = {}; ULONG fetched = 0;
    HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(result) || fetched != 1) return E_FAIL;
    WCHAR buffer[32]; ULONG copied = 0;
    result = selection.range->GetText(cookie, 0, buffer, 32, &copied);
    Check(SUCCEEDED(result) && std::u16string(buffer, buffer + copied) == u"\u05e9\u05dc\u05d5\u05dd",
          "a Windows TSF edit session reads the existing selected UTF-16 text");
    ITfContextComposition* owner = nullptr;
    ITfComposition* composition = nullptr;
    if (SUCCEEDED(result)) result = context_->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&owner));
    if (SUCCEEDED(result)) result = owner->StartComposition(cookie, selection.range,
        static_cast<ITfCompositionSink*>(this), &composition);
    Check(SUCCEEDED(result) && composition != nullptr, "the test text service starts reconversion on an existing range");
    if (SUCCEEDED(result)) result = selection.range->SetText(cookie, 0, L"\u65e5\u672c", 2);
    if (composition != nullptr) {
      HRESULT ended = composition->EndComposition(cookie);
      if (SUCCEEDED(result)) result = ended;
      composition->Release();
    }
    if (SUCCEEDED(result)) result = selection.range->Collapse(cookie, TF_ANCHOR_END);
    if (SUCCEEDED(result)) result = context_->SetSelection(cookie, 1, &selection);
    Check(SUCCEEDED(result), "the TSF service replaces/reconverts the selection and ends its composition");
    if (owner != nullptr) owner->Release();
    selection.range->Release();
    return result;
  }
  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition*) override { return S_OK; }
 private:
  LONG references_ = 1;
  ITfContext* context_;
};

class ReconversionService final : public ITfTextInputProcessor {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfTextInputProcessor)) {
      *out = static_cast<ITfTextInputProcessor*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG count = InterlockedDecrement(&references_);
    if (count == 0) { Deactivate(); delete this; }
    return static_cast<ULONG>(count);
  }
  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* manager, TfClientId client) override {
    if (manager == nullptr) return E_POINTER;
    Deactivate(); manager_ = manager; manager_->AddRef(); client_ = client; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Deactivate() override {
    if (manager_ != nullptr) manager_->Release();
    manager_ = nullptr; return S_OK;
  }
  HRESULT Reconvert() {
    ITfDocumentMgr* document = nullptr;
    ITfContext* context = nullptr;
    HRESULT result = manager_->GetFocus(&document);
    if (SUCCEEDED(result) && document != nullptr) result = document->GetTop(&context);
    if (SUCCEEDED(result) && context != nullptr) {
      auto* session = new ReconvertSession(context);
      HRESULT granted = E_FAIL;
      result = context->RequestEditSession(client_, session, TF_ES_SYNC | TF_ES_READWRITE, &granted);
      if (SUCCEEDED(result)) result = granted;
      session->Release();
    } else if (SUCCEEDED(result)) result = E_FAIL;
    if (context != nullptr) context->Release();
    if (document != nullptr) document->Release();
    return result;
  }
 private:
  LONG references_ = 1;
  ITfThreadMgr* manager_ = nullptr;
  TfClientId client_ = 0;  // TF_CLIENTID_NULL (absent from some mingw SDK headers)
};

#ifndef GESHER_TSF_SERVICE_DLL
void TestWindowsTextService(HWND window) {
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ITfThreadMgr* manager = nullptr;
  ITfDocumentMgr* document = nullptr;
  ITfContext* context = nullptr;
  TfClientId client = 0;
  TfEditCookie edit_cookie = 0;
  std::vector<DocumentEdit> edits;
  bool scheduled = false;
  auto* store = new TextStore(window, [&](DocumentEdit edit) { edits.push_back(std::move(edit)); }, [&] { scheduled = true; });
  TextDocument original; original.text = u"before \u05e9\u05dc\u05d5\u05dd";
  original.start = 7; original.end = 11; original.available = true; original.epoch = 10;
  store->Update(original);
  auto* service = new ReconversionService();
  bool activated = false;
  auto run = [&]() -> HRESULT {
    HRESULT result = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgr, reinterpret_cast<void**>(&manager));
    if (FAILED(result)) return result;
    result = manager->Activate(&client);
    if (FAILED(result)) return result;
    activated = true;
    result = manager->CreateDocumentMgr(&document);
    if (FAILED(result)) return result;
    result = document->CreateContext(client, 0, static_cast<ITextStoreACP*>(store), &context, &edit_cookie);
    if (FAILED(result)) return result;
    result = document->Push(context);
    if (FAILED(result)) return result;
    result = manager->SetFocus(document);
    if (FAILED(result)) return result;
    if (scheduled) { scheduled = false; store->Pump(); }
    result = service->Activate(manager, client);
    Check(SUCCEEDED(result), "an actual ITfTextInputProcessor activates against the Windows thread manager");
    if (FAILED(result)) return result;
    return service->Reconvert();
  };
  HRESULT result = FAILED(initialized) ? initialized : run();
  bool replaced = false, ended = false;
  for (const auto& edit : edits) {
    replaced |= edit.kind == DocumentEdit::kCompositionText && edit.text == u"\u65e5\u672c" && edit.start == 7 && edit.end == 11;
    ended |= edit.kind == DocumentEdit::kCompositionEnd && edit.text == u"\u65e5\u672c";
  }
  Check(SUCCEEDED(result) && replaced && ended,
        "Windows TSF delivers reconversion replacement and composition completion to the ACP document");
  if (FAILED(result)) std::printf("INFO  Windows TSF service HRESULT 0x%08lx\n", static_cast<unsigned long>(result));
  service->Release();
  if (manager != nullptr) manager->SetFocus(nullptr);
  if (document != nullptr) document->Pop(TF_POPF_ALL);
  if (context != nullptr) context->Release();
  if (document != nullptr) document->Release();
  if (activated) manager->Deactivate();
  if (manager != nullptr) manager->Release();
  store->Release();
  if (SUCCEEDED(initialized)) CoUninitialize();
}
#endif
}  // namespace
