// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
__declspec(dllimport) int gesher_dlopen_dependency(void);
__declspec(dllexport) int gesher_dlopen_value(void) {
  return gesher_dlopen_dependency() + 1;
}
