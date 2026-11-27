// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protected_profile_name.h"

namespace gesher::usb_storage {

// The data root is authority-owned input. Only the installer's canonical marker
// can authorize a wire owner. The caller retains the returned pin through ACL
// publication and process resume; it excludes writes, replacement and deletion.
inline DWORD PinProtectedOwnerMarker(const std::wstring& data_root,
                                     std::wstring_view package,
                                     std::wstring_view owner, HANDLE* output) {
  if (!output || *output || !protected_profile_name_detail::Package(package) ||
      !ValidInstallationOwner(owner))
    return ERROR_INVALID_PARAMETER;
  const std::wstring path =
      data_root + L"\\system\\package-enabled-owners\\" + std::wstring(package);
  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return GetLastError();
  const auto fail = [file](DWORD error) {
    CloseHandle(file);
    return error;
  };
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) return fail(GetLastError());
  if ((info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      info.nNumberOfLinks != 1 || info.nFileSizeHigh || info.nFileSizeLow != 36)
    return fail(ERROR_ACCESS_DENIED);
  char bytes[36]{};
  DWORD received = 0;
  if (!ReadFile(file, bytes, sizeof(bytes), &received, nullptr))
    return fail(GetLastError());
  if (received != sizeof(bytes)) return fail(ERROR_INVALID_DATA);
  for (size_t i = 0; i < sizeof(bytes); ++i)
    if (static_cast<unsigned char>(bytes[i]) != owner[i])
      return fail(ERROR_ACCESS_DENIED);
  std::wstring final(32768, L'\0');
  const DWORD count = GetFinalPathNameByHandleW(
      file, final.data(), static_cast<DWORD>(final.size()),
      FILE_NAME_NORMALIZED);
  if (!count || count >= final.size()) return fail(ERROR_INVALID_DATA);
  final.resize(count);
  const std::wstring expected = L"\\\\?\\" + path;
  if (CompareStringOrdinal(final.c_str(), -1, expected.c_str(), -1, TRUE) !=
      CSTR_EQUAL)
    return fail(ERROR_ACCESS_DENIED);
  // Parent paths may have harmless DOS casing, but two manifest package
  // spellings may not alias one case-insensitive marker and private data tree.
  wchar_t canonical[32768]{};
  const DWORD normalized = GetFinalPathNameByHandleW(
      file, canonical, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
  if (!normalized || normalized >= 32768) return fail(ERROR_INVALID_DATA);
  const std::wstring_view name(canonical, normalized);
  const size_t leaf = name.find_last_of(L'\\');
  if (leaf == std::wstring_view::npos || name.substr(leaf + 1) != package)
    return fail(ERROR_ACCESS_DENIED);
  *output = file;
  return ERROR_SUCCESS;
}

}  // namespace gesher::usb_storage
