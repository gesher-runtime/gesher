// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gesher::binder::data_pipe {

constexpr wchar_t kLocatorPrefix[] = LR"(\\.\pipe\gesher-private-binder-data-)";
constexpr size_t kLocatorHexDigits = 32;
// FILE_GENERIC_READ (including READ_CONTROL), FILE_WRITE_DATA and
// FILE_WRITE_ATTRIBUTES. FILE_CREATE_PIPE_INSTANCE is deliberately excluded.
constexpr uint32_t kClientAccess = 0x0012018b;

constexpr bool ValidLocator(std::wstring_view locator) {
  constexpr std::wstring_view prefix(kLocatorPrefix);
  if (!locator.starts_with(prefix) || locator.size() != prefix.size() + kLocatorHexDigits) {
    return false;
  }
  for (wchar_t ch : locator.substr(prefix.size())) {
    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'))) return false;
  }
  return true;
}

}  // namespace gesher::binder::data_pipe
