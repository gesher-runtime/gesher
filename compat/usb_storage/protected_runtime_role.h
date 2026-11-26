// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <stdbool.h>
#include <string.h>

// C JNI libraries use the executable's authenticated native role. A protected
// process must never fall back to an Android class name supplied in argv.
// -1: ordinary runtime; 0: protected application; 1: protected system_server.
static inline int GesherProtectedSystemServerRole(void) {
  typedef bool (*Query)(void);
  HMODULE executable = GetModuleHandleW(NULL);
  Query enabled = NULL, server = NULL;
  FARPROC address = GetProcAddress(executable, "gesher_protected_runtime_enabled");
#ifdef __cplusplus
  static_assert(sizeof(address) == sizeof(enabled));
#else
  _Static_assert(sizeof(address) == sizeof(enabled), "Windows function pointer representation");
#endif
  memcpy(&enabled, &address, sizeof(enabled));
  if (enabled == NULL || !enabled()) return -1;
  address = GetProcAddress(executable, "gesher_protected_runtime_system_server");
  memcpy(&server, &address, sizeof(server));
  return server != NULL && server() ? 1 : 0;
}
