// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../framework/app_container.h"

#include <string>
#include <string_view>

namespace gesher::usb_storage {

// Inputs come from the authority's sealed data root and verified process entry.
// No policy path or trustee is accepted from the application spawn environment.
inline DWORD GrantProtectedStoragePolicy(const std::wstring& data_root,
                                         std::wstring_view package, PSID app) {
  if (data_root.empty() || !app || package.empty() || package.size() > 255 ||
      package.front() == L'.' || package.back() == L'.' || package.find(L"..") != std::wstring_view::npos ||
      package.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") !=
          std::wstring_view::npos) return ERROR_INVALID_PARAMETER;
  const std::wstring directory = data_root + L"\\system\\storage-policy\\" + std::wstring(package);
  const DWORD attributes = GetFileAttributesW(directory.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return ERROR_ACCESS_DENIED;
  }
  const DWORD policy = GetFileAttributesW((directory + L"\\policy").c_str());
  if (policy == INVALID_FILE_ATTRIBUTES) return GetLastError();
  if (policy & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
  // Only this package's metadata directory. Children inherit read permission,
  // including each new staging file before the publisher atomically renames it.
  // Shared storage contents and other packages' policy directories get no grant.
  return appcontainer::Grant(directory, app, FILE_GENERIC_READ, true);
}

}  // namespace gesher::usb_storage
