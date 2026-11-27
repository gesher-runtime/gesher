// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protected_runtime_wire.h"
#include <windows.h>
#include <string>
#include <vector>
namespace gesher::usb_storage {
// The caller must first authenticate and authorize the enrolled system_server.
// This function only validates request syntax and derives sealed local paths;
// it cannot authenticate a process or grant access to a filesystem object.
inline bool ResolveLegacyArchivePaths(const std::wstring& data_root,
    const std::vector<std::byte>& payload, std::u16string* data,
    std::u16string* archive, std::u16string* binding) {
  runtime_wire::Reader reader(payload);
  std::u16string supplied_data, supplied_archive;
  if (!reader.Read(&supplied_data) || !reader.Read(&supplied_archive) ||
      !reader.Read(binding) || !reader.empty() || binding->size() != 64 ||
      binding->find_first_not_of(u"0123456789abcdef") != std::u16string::npos) return false;
  const auto same = [](const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
  };
  const std::wstring sealed_archive = data_root + L"\\system\\legacy-key-archive";
  if (!same(std::wstring(supplied_archive.begin(), supplied_archive.end()), sealed_archive)) return false;
  const std::wstring supplied(supplied_data.begin(), supplied_data.end());
  for (const auto& prefix : {data_root + L"\\data\\", data_root + L"\\user_de\\0\\"}) {
    if (supplied.size() <= prefix.size() || !same(supplied.substr(0, prefix.size()), prefix)) continue;
    const std::wstring package = supplied.substr(prefix.size());
    if (package.size() > 255 || package.front() == L'.' || package.back() == L'.' ||
        package.find(L"..") != package.npos || package.find_first_not_of(
        L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") != package.npos) return false;
    const std::wstring sealed_data = prefix + package;
    data->assign(sealed_data.begin(), sealed_data.end());
    archive->assign(sealed_archive.begin(), sealed_archive.end());
    return true;
  }
  return false;
}
}  // namespace gesher::usb_storage
