// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace gesher::usb_storage {

inline bool IsProtectedFilesystemTool(std::wstring_view path, std::wstring_view runtime) {
  if (path.empty() || path.size() >= 32768 || runtime.empty() || runtime.size() >= 32768 ||
      path.find(L'\0') != std::wstring_view::npos || runtime.find(L'\0') != std::wstring_view::npos) return false;
  // Exact absolute installed paths, not filename matches or a directory
  // prefix: no relative resolution, traversal, ADS or arbitrary package ELF.
  for (const wchar_t* file : {L"mkfs.fat", L"fsck.fat", L"mkfs.exfat", L"fsck.exfat", L"mke2fs", L"e2fsck"}) {
    const std::wstring expected = std::wstring(runtime) + L"\\filesystem-tools\\bin\\" + file;
    if (CompareStringOrdinal(path.data(), static_cast<int>(path.size()), expected.data(),
            static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL) return true;
  }
  return false;
}

inline bool ProtectedElfPolicy(std::wstring_view path, std::wstring_view runtime,
                               bool protected_runtime, bool credentials_valid,
                               uint32_t uid, uint32_t gid) {
  if (!protected_runtime) return true;
  if (!credentials_valid) return false;
  if (uid != 0 && gid != 0) return true;
  return IsProtectedFilesystemTool(path, runtime);
}

}  // namespace gesher::usb_storage
