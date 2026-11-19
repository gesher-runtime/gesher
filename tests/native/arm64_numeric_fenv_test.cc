// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Native helper verification. The companion ELF harness tests actual guest
// dispatch; this checks the numeric scope without an outer dispatcher scope
// that could otherwise hide failure to restore the native environment.
#include "../../compat/arm64/dll/numeric_fenv.h"

#include <cstdio>
#include <cstring>

#pragma STDC FENV_ACCESS ON

static unsigned checks, failures;
static void Check(bool ok, const char* item) {
  ++checks;
  if (!ok) { ++failures; std::printf("NUMERIC_FENV_FAIL %s\n", item); }
}

static uint64_t Bits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

int main() {
  std::fenv_t original;
  std::fegetenv(&original);
  const unsigned original_mxcsr = _mm_getcsr();
  constexpr int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  constexpr int exceptions[] = {FE_INVALID, FE_DIVBYZERO, FE_OVERFLOW, FE_UNDERFLOW, FE_INEXACT};
  constexpr uint32_t arm_exceptions[] = {1, 2, 4, 8, 16};
  for (int host_mode : modes) {
    for (uint32_t guest_mode = 0; guest_mode < 4; ++guest_mode) {
      std::fesetenv(FE_DFL_ENV);
      std::fesetround(host_mode);
      std::feraiseexcept(FE_INVALID | FE_DIVBYZERO);
      std::fenv_t before{}, after{};
      std::fegetenv(&before);
      const unsigned before_mxcsr = _mm_getcsr();
      {
        gesher_arm64::NumericFenvScope scope{guest_mode << 22};
        Check(std::fegetround() == modes[guest_mode], "guest rounding installed");
        Check(scope.RaisedArmExceptions() == 0, "native prior flags isolated");
        volatile double one = 1.0;
        volatile double halfway = 0x1p-53;
        const double positive = one + halfway;
        const double negative = -one - halfway;
        Check(Bits(positive) == (guest_mode == 1 ? UINT64_C(0x3ff0000000000001)
                                               : UINT64_C(0x3ff0000000000000)),
              "native positive arithmetic uses guest rounding");
        Check(Bits(negative) == (guest_mode == 2 ? UINT64_C(0xbff0000000000001)
                                               : UINT64_C(0xbff0000000000000)),
              "native negative arithmetic uses guest rounding");
        Check((scope.RaisedArmExceptions() & 16) != 0, "actual native inexact observed");
        for (unsigned i = 0; i < 5; ++i) {
          std::feclearexcept(FE_ALL_EXCEPT);
          std::feraiseexcept(exceptions[i]);
          Check((scope.RaisedArmExceptions() & arm_exceptions[i]) != 0,
                "native exception maps to ARM flag");
        }
      }
      std::fegetenv(&after);
      Check(std::memcmp(&before, &after, sizeof before) == 0, "complete native fenv restored");
      Check(_mm_getcsr() == before_mxcsr, "exact native MXCSR restored");
    }
  }
  std::fesetenv(&original);
  _mm_setcsr(original_mxcsr);
  std::printf("NUMERIC_FENV_DONE checks=%u failures=%u\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
