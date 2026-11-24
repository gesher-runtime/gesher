// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Gesher (devices track, ADR 0094 phase 2b): what com.gesher.server.WindowsHello
// needs besides the verification itself: Windows' build number, and where the
// foreground window is relative to the window that owns Windows Hello's prompt.
#pragma once

#include <windows.h>

namespace gesher::devices::hello {

// Windows' build number from RtlGetVersion (which no manifest changes, unlike
// GetVersionEx), or 0 when ntdll does not provide it.
int WindowsBuild();

// com.gesher.server.OwnerWatch's values.
enum class Relation : int {
  kOwner = 0,  // `foreground` belongs to the owner's process
  kOwned = 1,  // another process's window whose root owner is `owner`: Windows' prompt
  kOther = 2,  // neither
  kGone = 3,   // `owner` is not a window of process `pid` any more
  kNone = 4,   // no foreground window
};

// Classifies `foreground` relative to `owner`, a top-level window that belonged
// to process `pid` when the prompt started.
Relation RelationOf(HWND owner, DWORD pid, HWND foreground);

// RelationOf for Windows' current foreground window.
Relation ForegroundRelation(HWND owner, DWORD pid);

}  // namespace gesher::devices::hello
