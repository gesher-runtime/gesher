/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>

namespace gesher_arm64 {

// CNTFRQ_EL0 is 32-bit, while Windows reports a 64-bit QPC frequency.
// Keep the ordinary direct-QPC path; normalize only a frequency that cannot
// be advertised in CNTFRQ_EL0. QPC frequencies are positive on supported hosts.
constexpr std::uint32_t GuestCounterFrequency(std::uint64_t host_frequency) {
  return host_frequency <= 0xffffffffULL ? static_cast<std::uint32_t>(host_frequency)
                                        : 1000000000u;
}

constexpr std::uint64_t GuestCounterValue(std::uint64_t host_ticks,
                                        std::uint64_t host_frequency) {
  if (host_frequency <= 0xffffffffULL) return host_ticks;
  // A full-width QPC value times 1 GHz need not fit in 64 bits.
  return static_cast<std::uint64_t>(
      (static_cast<unsigned __int128>(host_ticks) * GuestCounterFrequency(host_frequency)) /
      host_frequency);
}

}  // namespace gesher_arm64
