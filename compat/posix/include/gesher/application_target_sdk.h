// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <stdint.h>

namespace gesher {

// Shared by ART and the ELF/bionic compatibility paths in the main PE.
// Standalone native executables start with current behavior.
inline std::atomic<uint32_t> application_target_sdk{36};

inline void SetApplicationTargetSdkVersion(uint32_t version) {
  application_target_sdk.store(version, std::memory_order_relaxed);
}

inline uint32_t ApplicationTargetSdkVersion() {
  return application_target_sdk.load(std::memory_order_relaxed);
}

}  // namespace gesher
