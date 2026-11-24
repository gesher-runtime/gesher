// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Isolated role-query fixture. These exports model the native identity boundary;
// they neither register a protected process nor obtain a production capability.
#include "../../compat/usb_storage/protected_runtime_role.h"
#include <stdio.h>

static bool enabled;
__declspec(dllexport) bool gesher_protected_runtime_enabled(void) { return enabled; }
#ifndef GESHER_TEST_MISSING_SERVER_QUERY
static bool server;
__declspec(dllexport) bool gesher_protected_runtime_system_server(void) { return server; }
#endif

int main(void) {
  if (GesherProtectedSystemServerRole() != -1) return 1;
  enabled = true;
  if (GesherProtectedSystemServerRole() != 0) return 2;
#ifdef GESHER_TEST_MISSING_SERVER_QUERY
  if (GesherProtectedSystemServerRole() != 0) return 3;
#else
  server = true;
  if (GesherProtectedSystemServerRole() != 1) return 3;
#endif
  enabled = false;
  if (GesherProtectedSystemServerRole() != -1) return 4;
  puts("Protected native role queries: 4 checks; explicit executable-query fixture");
  return 0;
}
