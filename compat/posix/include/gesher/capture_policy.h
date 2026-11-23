// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

namespace gesher::capture_policy {

enum class Kind : uint32_t { kMicrophone = 1, kCamera = 2 };

struct Request {
  uint32_t uid;
  Kind kind;
};
static_assert(sizeof(Request) == 8);

inline bool Valid(Kind kind) {
  return kind == Kind::kMicrophone || kind == Kind::kCamera;
}

}  // namespace gesher::capture_policy
