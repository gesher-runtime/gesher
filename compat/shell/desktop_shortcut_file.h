// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <string>
#include <vector>

namespace gesher::shell {

// Save the identity of the exact shortcut Gesher created. A name reused by the
// user is not evidence of ownership. The digest and deletion use one handle.
inline std::wstring DesktopShortcutReceipt(HANDLE file) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_ID_INFO identity{};
  LARGE_INTEGER size{};
  if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      !GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity)) ||
      !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) return {};
  const DWORD length = static_cast<DWORD>(size.QuadPart);
  std::vector<UCHAR> bytes(length);
  DWORD read = 0;
  if (!ReadFile(file, bytes.data(), length, &read, nullptr) || read != length) return {};
  UCHAR digest[32];
  BCRYPT_ALG_HANDLE provider = nullptr;
  if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  const NTSTATUS result = BCryptHash(provider, nullptr, 0, bytes.data(), length, digest, sizeof(digest));
  BCryptCloseAlgorithmProvider(provider, 0);
  if (result < 0) return {};
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring text;
  const auto append = [&](UCHAR byte) {
    text.push_back(hex[byte >> 4]);
    text.push_back(hex[byte & 15]);
  };
  for (UCHAR byte : digest) append(byte);
  for (UCHAR byte : identity.FileId.Identifier) append(byte);
  for (size_t i = 0; i < sizeof(identity.VolumeSerialNumber); ++i)
    append(static_cast<UCHAR>(identity.VolumeSerialNumber >> (8 * i)));
  return text;
}

inline bool CheckDesktopShortcut(const wchar_t* path, const std::wstring& receipt, bool remove) {
  constexpr size_t receipt_length = 2 * (32 + sizeof(FILE_ID_INFO));
  if (receipt.size() != receipt_length || receipt.find_first_not_of(L"0123456789abcdef") != std::wstring::npos)
    return false;
  HANDLE file = CreateFileW(path, GENERIC_READ | (remove ? DELETE : 0), FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  bool owned = DesktopShortcutReceipt(file) == receipt;
  if (owned && remove) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    owned = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
  }
  CloseHandle(file);
  return owned;
}

}  // namespace gesher::shell
