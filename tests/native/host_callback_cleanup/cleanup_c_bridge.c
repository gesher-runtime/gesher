// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cleanup_probe.h"

// A real C callback frame with no owned resource or language cleanup.
__declspec(noinline) uint64_t CleanupCBridge(CleanupLink* link) {
  uint64_t value = link->callback(link);
  link->notify(link->state, CLEANUP_AFTER, link->level * 100 + 31, 1);
  return value;
}

// Deliberately foreign to LLVM's C++ personality. The separate characterization
// must not equate this Windows SEH finally with a qualified LLVM RAII cleanup.
__declspec(noinline) uint64_t CleanupForeignFinally(CleanupLink* link,
                                                   CleanupCallback callback) {
  HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
  uint64_t value = 0;
  link->notify(link->state, CLEANUP_FOREIGN_OPEN, 90, event != NULL);
  __try {
    value = callback(link);
  } __finally {
    int ok = CloseHandle(event) != 0;
    link->notify(link->state, CLEANUP_FOREIGN_CLOSE, 90, ok);
  }
  return value;
}
