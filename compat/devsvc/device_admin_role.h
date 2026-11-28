// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cwchar>

// Follow ART's main-class boundary, including -XXlib: interleaved with a
// pending classpath value. Neither an option value nor an app extra is a role.
static inline bool GesherDeviceAdminServerMain(int count, const wchar_t* const* args) {
  if (args == nullptr || count < 2) return false;
  bool classpath_value = false;
  for (int i = 1; i < count; ++i) {
    if (args[i] == nullptr) return false;
    if (!classpath_value && args[i][0] != L'-')
      return std::wcscmp(args[i], L"com.gesher.server.SystemServer") == 0;
    if (std::wcsncmp(args[i], L"-XXlib:", 7) == 0) continue;
    classpath_value = std::wcscmp(args[i], L"-cp") == 0
        || std::wcscmp(args[i], L"-classpath") == 0;
  }
  return false;
}
