/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drop_target.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>

namespace gesher {
namespace windows {

// Registers OLE drag and drop for a native window.
// Ensures OLE is initialized on the calling thread.
bool EnableWindowDragDrop(HWND hwnd) {
  if (hwnd == nullptr) return false;
  HRESULT hr = RegisterWindowDropTarget(hwnd);
  return SUCCEEDED(hr);
}

// Disables OLE drag and drop for a native window.
void DisableWindowDragDrop(HWND hwnd) {
  if (hwnd == nullptr) return;
  RevokeWindowDropTarget(hwnd);
}

}  // namespace windows
}  // namespace gesher
