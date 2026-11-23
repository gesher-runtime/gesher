// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include <array>

namespace gesher::capture_policy {

enum class Kind : uint32_t {
  kMicrophone = 1, kCameraStart, kCameraCheck, kCameraFinish, kCameraReset
};

struct Attribution {
  static constexpr uint16_t kNone = UINT16_MAX;
  uint16_t length = kNone;
  std::array<char16_t, 50> tag{};

  bool valid() const { return length == kNone || length <= tag.size(); }
};
static_assert(sizeof(Attribution) == 102);

struct Request {
  uint32_t uid;
  Kind kind;
  std::array<uint8_t, 16> installation{};
  uint64_t session = 0;
  Attribution attribution{};
};
static_assert(sizeof(Request) == 136);

}  // namespace gesher::capture_policy
