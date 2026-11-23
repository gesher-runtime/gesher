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

#include <gesher/windows_process_identity.h>

namespace gesher::windows {

// Must run before the dedicated process exports any COM interface.
HRESULT ConfigureDropWorkerCom();

// A Windows caller observed inside an incoming OLE call. The broker obtains
// this from COM before inspecting IDataObject or opening any proposed paths.
class OleDropSource {
 public:
  OleDropSource() = default;
  ~OleDropSource();
  OleDropSource(const OleDropSource&) = delete;
  OleDropSource& operator=(const OleDropSource&) = delete;

  bool Capture();

  // The returned handle belongs to the caller. Directories are opened for
  // read-only enumeration. Only seekable file-system objects are returned.
  HANDLE Open(const wchar_t* path) const;

 private:
  HANDLE token_ = nullptr;
};

// Register/revoke on the same pumping STA. The transport must provide the
// exact retained process handle of the admitted guest and a target owned by
// this STA. No callback from the guest decides what the host can open.
class HostedDropWindow {
 public:
  HostedDropWindow() = default;
  ~HostedDropWindow();
  HostedDropWindow(const HostedDropWindow&) = delete;
  HostedDropWindow& operator=(const HostedDropWindow&) = delete;

  HRESULT Register(HWND hwnd, HANDLE guest_process, IDropTarget* target);
  bool Live() const;
  void Revoke();

 private:
  HWND hwnd_ = nullptr;
  HANDLE process_ = nullptr;
  DWORD thread_id_ = 0;
  DWORD owner_thread_ = 0;
};

}  // namespace gesher::windows
