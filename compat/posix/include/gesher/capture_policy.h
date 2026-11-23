// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include <array>

namespace gesher::capture_policy {

enum class Kind : uint32_t {
  kMicrophone = 1, kCameraStart, kCameraCheck, kCameraFinish, kCameraReset
};

struct Request {
  uint32_t uid;
  Kind kind;
  std::array<uint8_t, 16> installation{};
  uint64_t session = 0;
};
static_assert(sizeof(Request) == 32);

}  // namespace gesher::capture_policy
