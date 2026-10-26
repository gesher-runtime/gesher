// TSF's ACP document over an InputConnection snapshot. No ART/AOSP headers:
// the COM locking/edit contract can be exercised by a small native service.
#pragma once
#include <windows.h>
#include <textstor.h>
#include <msctf.h>
#include <olectl.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include "text_document.h"

namespace gesher::input {
// All methods run on the window's STA. The owner posts snapshots and schedules
// Pump() through its message loop. Notifications never echo edits made by TSF.
class TextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink {
 public:
  using Emit = std::function<void(DocumentEdit)>;
  using Schedule = std::function<void()>;
  TextStore(HWND window, Emit emit, Schedule schedule);
  void Update(TextDocument document);
  void Caret(const RECT& screen);
  void Pump();
  void Disconnect();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID, IUnknown*, DWORD) override;
  HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown*) override;
  HRESULT STDMETHODCALLTYPE RequestLock(DWORD, HRESULT*) override;
  HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS*) override;
  HRESULT STDMETHODCALLTYPE QueryInsert(LONG, LONG, ULONG, LONG*, LONG*) override;
  HRESULT STDMETHODCALLTYPE GetSelection(ULONG, ULONG, TS_SELECTION_ACP*, ULONG*) override;
  HRESULT STDMETHODCALLTYPE SetSelection(ULONG, const TS_SELECTION_ACP*) override;
  HRESULT STDMETHODCALLTYPE GetText(LONG, LONG, WCHAR*, ULONG, ULONG*, TS_RUNINFO*, ULONG, ULONG*, LONG*) override;
  HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG, LONG, const WCHAR*, ULONG, TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject**) override;
  HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override;
  HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL*) override;
  HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD, const WCHAR*, ULONG, LONG*, LONG*, TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override;
  HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD, LONG*, BOOL*, LONG*) override;
  HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG*) override;
  HRESULT STDMETHODCALLTYPE GetEndACP(LONG*) override;
  HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie*) override;
  HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG*) override;
  HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG, LONG, RECT*, BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT*) override;
  HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND*) override;
  HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*, BOOL*) override;
  HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*, ITfRange*) override;
  HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override;
 private:
  friend struct TextServiceProbeAccess;
  ~TextStore();
  bool ReadLock() const;
  bool WriteLock() const;
  bool Range(LONG, LONG) const;
  void Publish(DocumentEdit);
  void Apply(TextDocument);
  HWND window_;
  Emit emit_;
  Schedule schedule_;
  LONG references_ = 1;
  ITextStoreACPSink* sink_ = nullptr;
  DWORD sink_mask_ = 0, lock_ = 0, pending_lock_ = 0;
  bool pumping_ = false, composing_ = false, pending_layout_ = false;
  int32_t revision_ = 0;
  LONG composition_start_ = 0, composition_end_ = 0;
  TextDocument document_;
  std::optional<TextDocument> pending_document_;
  RECT caret_ = {};
};
}  // namespace gesher::input
