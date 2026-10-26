#include "text_store.h"
#include <algorithm>
#include <climits>
#include <utility>

namespace gesher::input {
TextStore::TextStore(HWND window, Emit emit, Schedule schedule)
    : window_(window), emit_(std::move(emit)), schedule_(std::move(schedule)) {}
TextStore::~TextStore() { if (sink_ != nullptr) sink_->Release(); }
void TextStore::Disconnect() {
  // A manager retaining a closing COM object must never call its former
  // window/Java owner or retain that editor's text.
  emit_ = [](DocumentEdit) {};
  schedule_ = [] {};
  document_ = TextDocument{}; pending_document_.reset();
  pending_lock_ = 0; pending_layout_ = false; composing_ = false;
  window_ = nullptr;
}
HRESULT TextStore::QueryInterface(REFIID iid, void** out) {
  if (out == nullptr) return E_POINTER;
  *out = nullptr;
  if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITextStoreACP)) {
    *out = static_cast<ITextStoreACP*>(this);
  } else if (IsEqualIID(iid, IID_ITfContextOwnerCompositionSink)) {
    *out = static_cast<ITfContextOwnerCompositionSink*>(this);
  } else return E_NOINTERFACE;
  AddRef(); return S_OK;
}
ULONG TextStore::AddRef() { return InterlockedIncrement(&references_); }
ULONG TextStore::Release() {
  LONG count = InterlockedDecrement(&references_);
  if (count == 0) delete this;
  return static_cast<ULONG>(count);
}
namespace {
bool SameIdentity(IUnknown* left, IUnknown* right) {
  IUnknown* a = nullptr; IUnknown* b = nullptr;
  if (left != nullptr) left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&a));
  if (right != nullptr) right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&b));
  bool same = a != nullptr && a == b;
  if (a != nullptr) a->Release();
  if (b != nullptr) b->Release();
  return same;
}
}
HRESULT TextStore::AdviseSink(REFIID iid, IUnknown* unknown, DWORD mask) {
  if (unknown == nullptr) return E_POINTER;
  if (!IsEqualIID(iid, IID_ITextStoreACPSink)) return E_INVALIDARG;
  if (sink_ != nullptr) {
    if (!SameIdentity(sink_, unknown)) return CONNECT_E_ADVISELIMIT;
    sink_mask_ = mask; return S_OK;
  }
  HRESULT result = unknown->QueryInterface(iid, reinterpret_cast<void**>(&sink_));
  if (SUCCEEDED(result)) sink_mask_ = mask;
  return result;
}
HRESULT TextStore::UnadviseSink(IUnknown* unknown) {
  if (!SameIdentity(sink_, unknown)) return CONNECT_E_NOCONNECTION;
  sink_->Release(); sink_ = nullptr; sink_mask_ = 0; return S_OK;
}
bool TextStore::ReadLock() const { return (lock_ & TS_LF_READ) != 0; }
bool TextStore::WriteLock() const { return (lock_ & TS_LF_READWRITE) == TS_LF_READWRITE; }
bool TextStore::Range(LONG start, LONG end) const {
  return start >= 0 && end >= start && static_cast<size_t>(end) <= document_.text.size();
}
HRESULT TextStore::RequestLock(DWORD flags, HRESULT* session) {
  if (session == nullptr) return E_POINTER;
  if (sink_ == nullptr) return E_UNEXPECTED;
  if ((flags & TS_LF_READ) == 0 || (flags & ~(TS_LF_SYNC | TS_LF_READWRITE)) != 0) return E_INVALIDARG;
  if (lock_ != 0 || pending_document_.has_value()) {
    if ((flags & TS_LF_SYNC) != 0) { *session = TS_E_SYNCHRONOUS; return S_OK; }
    pending_lock_ |= flags & TS_LF_READWRITE;
    *session = TS_S_ASYNC; schedule_(); return S_OK;
  }
  lock_ = flags & TS_LF_READWRITE;
  *session = sink_->OnLockGranted(lock_);
  lock_ = 0;
  if (pending_lock_ != 0 || pending_document_.has_value()) schedule_();
  return S_OK;
}
void TextStore::Pump() {
  if (lock_ != 0 || pumping_) return;
  pumping_ = true;
  if (pending_document_.has_value()) {
    TextDocument next = std::move(*pending_document_); pending_document_.reset(); Apply(std::move(next));
  }
  if (pending_layout_) {
    pending_layout_ = false;
    if (sink_ != nullptr && (sink_mask_ & TS_AS_LAYOUT_CHANGE) != 0)
      sink_->OnLayoutChange(TS_LC_CHANGE, 0);
  }
  DWORD pending = pending_lock_; pending_lock_ = 0;
  if (pending != 0 && sink_ != nullptr) {
    HRESULT ignored; RequestLock(pending, &ignored);
  }
  pumping_ = false;
}
void TextStore::Update(TextDocument document) {
  if (!document.available) {
    document.text.clear(); document.start = document.end = document.offset = 0;
    document.reversed = false;
  }
  if (document.text.size() > LONG_MAX || document.start < 0 || document.end < document.start ||
      static_cast<size_t>(document.end) > document.text.size() || document.offset < 0 ||
      static_cast<size_t>(document.offset) > LONG_MAX - document.text.size()) return;
  if (document.epoch < document_.epoch || (pending_document_.has_value() &&
      document.epoch < pending_document_->epoch)) return;
  // Do not replace optimistic TSF edits with an older asynchronous editor reply.
  if (document.epoch == document_.epoch && document.applied_revision < revision_) return;
  if (lock_ != 0 || pumping_) { pending_document_ = std::move(document); schedule_(); return; }
  Apply(std::move(document));
}
void TextStore::Apply(TextDocument document) {
  bool changed = document.text != document_.text || document.epoch != document_.epoch;
  bool selection = changed || document.start != document_.start || document.end != document_.end
      || document.reversed != document_.reversed;
  bool status = document.available != document_.available;
  TS_TEXTCHANGE delta = {0, static_cast<LONG>(document_.text.size()), static_cast<LONG>(document.text.size())};
  if (document.epoch != document_.epoch) { revision_ = 0; composing_ = false; }
  if (!document.available) composing_ = false;
  document_ = std::move(document);
  if (sink_ == nullptr) return;
  if (changed && (sink_mask_ & TS_AS_TEXT_CHANGE) != 0) sink_->OnTextChange(0, &delta);
  if (selection && (sink_mask_ & TS_AS_SEL_CHANGE) != 0) sink_->OnSelectionChange();
  if (status && (sink_mask_ & TS_AS_STATUS_CHANGE) != 0) sink_->OnStatusChange(TS_SD_READONLY);
}
void TextStore::Caret(const RECT& screen) {
  caret_ = screen;
  pending_layout_ = true; schedule_();
}
void TextStore::Publish(DocumentEdit edit) {
  edit.epoch = document_.epoch; edit.revision = ++revision_;
  edit.start += document_.offset; edit.end += document_.offset;
  emit_(std::move(edit));
}
HRESULT TextStore::GetStatus(TS_STATUS* status) {
  if (status == nullptr) return E_POINTER;
  status->dwDynamicFlags = document_.available ? 0 : TS_SD_READONLY;
  status->dwStaticFlags = TS_SS_NOHIDDENTEXT; return S_OK;
}
HRESULT TextStore::QueryInsert(LONG start, LONG end, ULONG count, LONG* first, LONG* last) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (first == nullptr || last == nullptr) return E_POINTER;
  if (!Range(start, end) || count > LONG_MAX || document_.text.size() - (end - start) > LONG_MAX - count)
    return TS_E_INVALIDPOS;
  *first = start; *last = end; return S_OK;
}
HRESULT TextStore::GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection, ULONG* fetched) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (selection == nullptr || fetched == nullptr) return E_POINTER;
  *fetched = 0;
  if ((index != 0 && index != TS_DEFAULT_SELECTION) || count == 0) return E_INVALIDARG;
  *selection = TS_SELECTION_ACP{document_.start, document_.end,
      {document_.reversed ? TS_AE_START : TS_AE_END, FALSE}};
  *fetched = 1; return S_OK;
}
HRESULT TextStore::SetSelection(ULONG count, const TS_SELECTION_ACP* selection) {
  if (!WriteLock()) return TS_E_NOLOCK;
  if (selection == nullptr) return E_POINTER;
  if (!document_.available) return TS_E_READONLY;
  if (count != 1 || !Range(selection->acpStart, selection->acpEnd) ||
      selection->style.ase < TS_AE_NONE || selection->style.ase > TS_AE_END) return TS_E_INVALIDPOS;
  if (revision_ == INT_MAX) return E_FAIL;
  document_.start = selection->acpStart; document_.end = selection->acpEnd;
  document_.reversed = selection->style.ase == TS_AE_START;
  DocumentEdit edit; edit.kind = DocumentEdit::kSelect;
  edit.start = document_.reversed ? document_.end : document_.start;
  edit.end = document_.reversed ? document_.start : document_.end;
  Publish(std::move(edit)); return S_OK;
}
HRESULT TextStore::GetText(LONG start, LONG end, WCHAR* text, ULONG requested, ULONG* copied,
                          TS_RUNINFO* runs, ULONG requested_runs, ULONG* returned_runs, LONG* next) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (copied == nullptr || returned_runs == nullptr || next == nullptr ||
      (requested != 0 && text == nullptr) || (requested_runs != 0 && runs == nullptr)) return E_POINTER;
  *copied = *returned_runs = 0;
  if (end == -1) end = static_cast<LONG>(document_.text.size());
  if (!Range(start, end)) return TS_E_INVALIDPOS;
  ULONG count = requested != 0 ? std::min(requested, static_cast<ULONG>(end - start))
                              : (requested_runs != 0 ? static_cast<ULONG>(end - start) : 0);
  if (requested != 0) {
    std::copy_n(document_.text.data() + start, count, text); *copied = count;
  }
  if (requested_runs != 0 && count != 0) { runs[0] = TS_RUNINFO{count, TS_RT_PLAIN}; *returned_runs = 1; }
  *next = start + count; return S_OK;
}
HRESULT TextStore::SetText(DWORD, LONG start, LONG end, const WCHAR* text, ULONG count, TS_TEXTCHANGE* change) {
  if (!WriteLock()) return TS_E_NOLOCK;
  if (change == nullptr || (count != 0 && text == nullptr)) return E_POINTER;
  if (!document_.available) return TS_E_READONLY;
  if (revision_ == INT_MAX) return E_FAIL;
  if (!Range(start, end) || count > LONG_MAX || document_.text.size() - (end - start) > LONG_MAX - count)
    return TS_E_INVALIDPOS;
  std::u16string inserted;
  if (count != 0) inserted.assign(text, text + count);
  document_.text.replace(start, end - start, inserted);
  document_.start = document_.end = start + static_cast<LONG>(count);
  document_.reversed = false;
  *change = TS_TEXTCHANGE{start, end, document_.end};
  DocumentEdit edit; edit.kind = DocumentEdit::kReplace; edit.start = start; edit.end = end;
  edit.text = std::move(inserted); Publish(std::move(edit)); return S_OK;
}
HRESULT TextStore::InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG count,
                                       LONG* start, LONG* end, TS_TEXTCHANGE* change) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if ((flags & TS_IAS_NOQUERY) != 0 && (flags & TS_IAS_QUERYONLY) != 0) return E_INVALIDARG;
  if ((flags & ~(TS_IAS_NOQUERY | TS_IAS_QUERYONLY)) != 0) return E_INVALIDARG;
  if ((flags & TS_IAS_NOQUERY) == 0 && (start == nullptr || end == nullptr)) return E_POINTER;
  if (count > LONG_MAX || document_.text.size() - (document_.end - document_.start) > LONG_MAX - count)
    return TS_E_INVALIDPOS;
  LONG cursor = document_.start + static_cast<LONG>(count);
  if (start != nullptr) *start = cursor;
  if (end != nullptr) *end = cursor;
  if ((flags & TS_IAS_QUERYONLY) != 0) return S_OK;
  return SetText(0, document_.start, document_.end, text, count, change);
}
HRESULT TextStore::GetFormattedText(LONG, LONG, IDataObject** out) { if (out) *out = nullptr; return E_NOTIMPL; }
HRESULT TextStore::GetEmbedded(LONG, REFGUID, REFIID, IUnknown** out) { if (out) *out = nullptr; return E_NOTIMPL; }
HRESULT TextStore::QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* okay) { if (!okay) return E_POINTER; *okay = FALSE; return S_OK; }
HRESULT TextStore::InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) { return E_NOTIMPL; }
HRESULT TextStore::InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) { return E_NOTIMPL; }
HRESULT TextStore::RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) { return S_OK; }
HRESULT TextStore::RequestAttrsAtPosition(LONG position, ULONG, const TS_ATTRID*, DWORD) { return Range(position, position) ? S_OK : TS_E_INVALIDPOS; }
HRESULT TextStore::RequestAttrsTransitioningAtPosition(LONG position, ULONG, const TS_ATTRID*, DWORD) { return Range(position, position) ? S_OK : TS_E_INVALIDPOS; }
HRESULT TextStore::FindNextAttrTransition(LONG start, LONG halt, ULONG, const TS_ATTRID*, DWORD,
                                        LONG* next, BOOL* found, LONG* offset) {
  if (!next || !found || !offset) return E_POINTER;
  if (!Range(start, halt)) return TS_E_INVALIDPOS;
  *next = halt; *found = FALSE; *offset = 0; return S_OK;
}
HRESULT TextStore::RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) { if (!fetched) return E_POINTER; *fetched = 0; return S_OK; }
HRESULT TextStore::GetEndACP(LONG* end) { if (!ReadLock()) return TS_E_NOLOCK; if (!end) return E_POINTER; *end = static_cast<LONG>(document_.text.size()); return S_OK; }
HRESULT TextStore::GetActiveView(TsViewCookie* view) { if (!view) return E_POINTER; *view = 0; return S_OK; }
HRESULT TextStore::GetWnd(TsViewCookie view, HWND* window) { if (!window) return E_POINTER; if (view != 0) return E_INVALIDARG; *window = window_; return S_OK; }
HRESULT TextStore::GetScreenExt(TsViewCookie view, RECT* rectangle) {
  if (!rectangle) return E_POINTER;
  if (view != 0) return E_INVALIDARG;
  if (!GetClientRect(window_, rectangle)) return E_FAIL;
  MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(rectangle), 2); return S_OK;
}
HRESULT TextStore::GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rectangle, BOOL* clipped) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (!rectangle || !clipped) return E_POINTER;
  if (view != 0 || !Range(start, end)) return TS_E_INVALIDPOS;
  if (start != end || start != document_.end || caret_.bottom <= caret_.top) return TS_E_NOLAYOUT;
  *rectangle = caret_; *clipped = FALSE; return S_OK;
}
HRESULT TextStore::GetACPFromPoint(TsViewCookie view, const POINT* point, DWORD, LONG* position) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (!point || !position) return E_POINTER;
  if (view != 0) return E_INVALIDARG;
  if (!PtInRect(&caret_, *point)) return TS_E_NOLAYOUT;
  *position = document_.end; return S_OK;
}
HRESULT TextStore::OnStartComposition(ITfCompositionView*, BOOL* okay) {
  if (!okay) return E_POINTER;
  *okay = document_.available; composing_ = *okay != FALSE; return S_OK;
}
HRESULT TextStore::OnUpdateComposition(ITfCompositionView* composition, ITfRange* range) {
  if (!composing_) return S_OK;
  ITfRange* owned = nullptr;
  if (range == nullptr && composition != nullptr && SUCCEEDED(composition->GetRange(&owned))) range = owned;
  ITfRangeACP* acp = nullptr;
  HRESULT result = range == nullptr ? E_INVALIDARG : range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
  if (SUCCEEDED(result)) {
    LONG start = 0, length = 0; result = acp->GetExtent(&start, &length);
    if (SUCCEEDED(result) && start >= 0 && length >= 0 &&
        static_cast<size_t>(start) <= document_.text.size() &&
        static_cast<size_t>(length) <= document_.text.size() - start && revision_ != INT_MAX) {
      DocumentEdit edit; edit.kind = DocumentEdit::kCompositionRange; edit.start = start; edit.end = start + length;
      Publish(std::move(edit));
    }
    acp->Release();
  }
  if (owned != nullptr) owned->Release();
  return result;
}
HRESULT TextStore::OnEndComposition(ITfCompositionView*) {
  if (composing_) {
    composing_ = false;
    if (revision_ == INT_MAX) return E_FAIL;
    DocumentEdit edit; edit.kind = DocumentEdit::kCompositionEnd; Publish(std::move(edit));
  }
  return S_OK;
}
}  // namespace gesher::input
