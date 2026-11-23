// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace gesher::binder {

enum class PeerRole : uint32_t {
  kClient = 0,
  kContextManager = 1,
};

constexpr bool IsAllowedPeerRole(uint32_t uid, PeerRole role) {
  constexpr uint32_t kAndroidSystemUid = 1000;
  return role == PeerRole::kClient ||
      (role == PeerRole::kContextManager && uid == kAndroidSystemUid);
}

}  // namespace gesher::binder
