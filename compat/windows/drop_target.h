/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <vector>

namespace gesher {
namespace windows {

// Registers an OLE IDropTarget for `hwnd` with RegisterDragDrop.
// Returns S_OK on success or an OLE error code.
HRESULT RegisterWindowDropTarget(HWND hwnd);

// Revokes any registered IDropTarget on `hwnd` with RevokeDragDrop.
HRESULT RevokeWindowDropTarget(HWND hwnd);

// Gesher's OLE IDropTarget implementation for Windows desktop integration.
// Handles CF_HDROP (files) and CF_TEXT / CF_UNICODETEXT (text), and
// translates them into android.view.DragEvent dispatched to the focused
// Android window session.
class GesherDropTarget : public IDropTarget {
 public:
  explicit GesherDropTarget(HWND hwnd);
  virtual ~GesherDropTarget();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IDropTarget
  STDMETHODIMP DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt,
                         DWORD* pdwEffect) override;
  STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
  STDMETHODIMP DragLeave() override;
  STDMETHODIMP Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt,
                    DWORD* pdwEffect) override;

 private:
  std::atomic<ULONG> ref_count_{1};
  HWND hwnd_ = nullptr;
  bool format_supported_ = false;

  bool QuerySupported(IDataObject* pDataObj);
  void ExtractAndDispatchDrop(IDataObject* pDataObj, POINT client_pt);
};

// Dispatches a drag event to the Android WindowSession for the given window handle.
void DispatchDragToSession(HWND hwnd, int action, float x, float y,
                           const std::vector<std::wstring>* files,
                           const std::wstring& text);

}  // namespace windows
}  // namespace gesher
