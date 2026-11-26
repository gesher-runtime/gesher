// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace gesher::net {

// Facts reported by the Network List Manager for one adapter. The metering bit
// is conservative: it withholds Android's positive NOT_METERED capability until
// Windows explicitly confirms unrestricted cost. It does not invent a tariff.
struct NlmInfo {
  // NLM_CONNECTION_COST values; windows_networks.cc checks these against the SDK.
  static constexpr std::uint32_t kUnrestricted = 0x1;
  static constexpr std::uint32_t kFixed = 0x2;
  static constexpr std::uint32_t kVariable = 0x4;
  static constexpr std::uint32_t kCongested = 0x20000;
  static constexpr std::uint32_t kRoaming = 0x40000;

  bool internet = false;
  bool captive_portal = false;
  bool metered = true;
  bool roaming = false;
  bool congested = false;

  void SetCost(bool available, std::uint32_t value) {
    // Unknown (0), a failed query, or contradictory base cost bits cannot prove
    // the connection is unrestricted. Reset even an existing adapter entry.
    metered = !available || (value & (kUnrestricted | kFixed | kVariable)) != kUnrestricted;
    if (available) {
      roaming = (value & kRoaming) != 0;
      congested = (value & kCongested) != 0;
    }
  }
};

}  // namespace gesher::net
