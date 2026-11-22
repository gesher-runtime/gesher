// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cfenv>
#include <cstdint>
#include <xmmintrin.h>

namespace gesher_arm64 {

// The strtold backend uses native binary64 seed arithmetic as well as its
// explicit rounding mode. Isolate that arithmetic from the native caller's
// flags/trap enables, and install the rounding field of the guest FPCR.
// This scope covers the standard nontrapping numeric environment; it does
// not claim to emulate additional FPCR modes such as flush-to-zero.
class NumericFenvScope {
 public:
  explicit NumericFenvScope(uint32_t fpcr) : saved_mxcsr_(_mm_getcsr()) {
    constexpr int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    std::fegetenv(&saved_);
    std::fesetenv(FE_DFL_ENV);
    // The pinned MinGW default reset initializes x87 but leaves MXCSR intact.
    // Start SSE arithmetic with clear status, masked traps and no native FTZ/DAZ;
    // changing only the rounding field would retain flags when it is unchanged.
    _mm_setcsr(_MM_MASK_MASK);
    std::fesetround(modes[(fpcr >> 22) & 3]);
  }
  ~NumericFenvScope() {
    std::fesetenv(&saved_);
    _mm_setcsr(saved_mxcsr_);
  }
  NumericFenvScope(const NumericFenvScope&) = delete;
  NumericFenvScope& operator=(const NumericFenvScope&) = delete;

  uint32_t RaisedArmExceptions() const {
    const int raised = std::fetestexcept(FE_ALL_EXCEPT);
    uint32_t flags = 0;
    if (raised & FE_INVALID) flags |= 0x01;
    if (raised & FE_DIVBYZERO) flags |= 0x02;
    if (raised & FE_OVERFLOW) flags |= 0x04;
    if (raised & FE_UNDERFLOW) flags |= 0x08;
    if (raised & FE_INEXACT) flags |= 0x10;
    return flags;
  }

 private:
  std::fenv_t saved_;
  unsigned saved_mxcsr_;
};

}  // namespace gesher_arm64
