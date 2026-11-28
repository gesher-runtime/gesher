// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstring>
#include <string>
#include <utility>

namespace gesher::canonical_file_name {

// A lowbox can query a held file's normalized volume-relative name while the
// DOS-device query in GetFinalPathNameByHandleW is denied. A drive from the
// original lookup is only a candidate: reopening it with this token must prove
// the complete file identity. Never infer a drive from a volume-relative name.
inline bool RevalidatedDosName(HANDLE file, const std::wstring& original,
                               std::wstring* result) {
  if (original.size() >= 32768) return false;
  // MapPath uses extended DOS names for long paths and root-relative names
  // for the current drive. Resolve that drive through Windows; relative names,
  // UNC and other device namespaces are never suitable hints.
  const std::wstring input = original.starts_with(L"\\\\?\\") ? original.substr(4) : original;
  const auto slash = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
  const bool drive_absolute = input.size() >= 3 && input[1] == L':' && slash(input[2]) &&
      ((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z'));
  const bool drive_rooted = input.size() >= 1 && slash(input[0]) &&
      (input.size() == 1 || !slash(input[1]));
  if (!drive_absolute && !drive_rooted) return false;
  wchar_t absolute[32768];
  const DWORD full = GetFullPathNameW(input.c_str(), 32768, absolute, nullptr);
  if (full < 3 || full >= 32768 || absolute[1] != L':' || absolute[2] != L'\\' ||
      !((absolute[0] >= L'A' && absolute[0] <= L'Z') ||
        (absolute[0] >= L'a' && absolute[0] <= L'z'))) return false;
  wchar_t relative[32768];
  const DWORD length = GetFinalPathNameByHandleW(file, relative, 32768,
      FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
  if (length == 0 || length >= 32768 || relative[0] != L'\\' ||
      (length > 1 && relative[1] == L'\\') || length + 6 >= 32768) return false;
  std::wstring candidate(absolute, 2);
  candidate.append(relative, length);
  const std::wstring open_name = L"\\\\?\\" + candidate;
  HANDLE reopened = CreateFileW(open_name.c_str(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (reopened == INVALID_HANDLE_VALUE) return false;
  FILE_ID_INFO held{}, checked{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  const bool same = GetFileInformationByHandleEx(reopened, FileAttributeTagInfo,
          &attributes, sizeof(attributes)) &&
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
      GetFileInformationByHandleEx(file, FileIdInfo, &held, sizeof(held)) &&
      GetFileInformationByHandleEx(reopened, FileIdInfo, &checked, sizeof(checked)) &&
      held.VolumeSerialNumber == checked.VolumeSerialNumber &&
      std::memcmp(held.FileId.Identifier, checked.FileId.Identifier,
                  sizeof(held.FileId.Identifier)) == 0;
  CloseHandle(reopened);
  if (same) *result = std::move(candidate);
  return same;
}

}  // namespace gesher::canonical_file_name
