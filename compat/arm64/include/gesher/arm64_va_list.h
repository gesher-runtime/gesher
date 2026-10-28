// Private translator/shim agreement, guarded by GESHER_ARM64_API_VERSION 3.
// This is never exposed as the guest's AAPCS64 va_list or an ordinary x86
// SysV register save area. Only the shared shim readers consume the tag.
#pragma once

#include <stdint.h>

namespace gesher::abi {

constexpr uint32_t kArm64VaTag = 0xa6400000u;
constexpr uint32_t kArm64VaTagMask = 0xffff0000u;
constexpr uint32_t kArm64VaOffsetMask = 0x0000ffffu;
constexpr uint32_t kArm64VaGpBytes = 64;
constexpr uint32_t kArm64VaFpBytes = 128;

// Immutable metadata shared by independent by-value copies of the 24-byte
// host list. The two bank offsets and common overflow cursor live in each
// list, so consuming a copy cannot move another copy's cursor. The guest's
// saved registers stay live until its host call returns, including callbacks.
struct Arm64VaRegisters {
  const uint8_t* gp_top;
  const uint8_t* fp_top;
};

}  // namespace gesher::abi
