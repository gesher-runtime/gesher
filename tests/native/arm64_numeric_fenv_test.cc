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
static int current_host_mode;
static uint32_t current_guest_mode;
static void Check(bool ok, const char* item) {
  ++checks;
  if (!ok) {
    ++failures;
    unsigned short x87_control, x87_status;
    __asm__ volatile("fnstcw %0" : "=m"(x87_control));
    __asm__ volatile("fnstsw %0" : "=m"(x87_status));
    std::printf("NUMERIC_FENV_FAIL %s host_round=%x guest_mode=%u mxcsr=%08x "
                "x87_control=%04x x87_status=%04x\n",
                item, current_host_mode, current_guest_mode, _mm_getcsr(),
                x87_control, x87_status);
  }
}

static uint64_t Bits(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

// Exercise inherited SSE status, trap masks and denormal controls directly.
// Capture the caller's restored state before returning to a safe environment
// for diagnostics; no arithmetic is performed with an inherited trap enabled.
static unsigned CheckNativeStateRestoration() {
  std::fenv_t original;
  std::fegetenv(&original);
  const unsigned original_mxcsr = _mm_getcsr();
  constexpr int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  constexpr unsigned sse_modes[] = {
      _MM_ROUND_NEAREST, _MM_ROUND_UP, _MM_ROUND_DOWN, _MM_ROUND_TOWARD_ZERO};
  constexpr unsigned denormals_are_zero = 0x0040;
  constexpr unsigned controls[] = {
      _MM_MASK_MASK | _MM_EXCEPT_MASK,
      _MM_MASK_MASK | _MM_FLUSH_ZERO_ON | denormals_are_zero | _MM_EXCEPT_MASK,
      (_MM_MASK_MASK & ~_MM_MASK_INVALID) | _MM_EXCEPT_INEXACT,
      (_MM_MASK_MASK & ~_MM_MASK_DIV_ZERO) | _MM_FLUSH_ZERO_ON |
          denormals_are_zero | _MM_EXCEPT_UNDERFLOW | _MM_EXCEPT_INEXACT};
  unsigned restoration_checks = 0, restoration_failures = 0;
  for (unsigned native_control : controls) {
    for (unsigned host_mode = 0; host_mode < 4; ++host_mode) {
      for (uint32_t guest_mode = 0; guest_mode < 4; ++guest_mode) {
        std::fesetenv(FE_DFL_ENV);
        std::fesetround(modes[host_mode]);
        const unsigned requested = native_control | sse_modes[host_mode];
        _mm_setcsr(requested);
        std::fenv_t before{}, after{};
        std::fegetenv(&before);
        const unsigned before_mxcsr = _mm_getcsr();
        unsigned scope_mxcsr;
        uint32_t scope_flags;
        int scope_round;
        {
          gesher_arm64::NumericFenvScope scope{guest_mode << 22};
          scope_mxcsr = _mm_getcsr();
          scope_flags = scope.RaisedArmExceptions();
          scope_round = std::fegetround();
        }
        std::fegetenv(&after);
        const unsigned after_mxcsr = _mm_getcsr();
        std::fesetenv(&original);
        _mm_setcsr(original_mxcsr);
        const auto verify = [&](bool ok, const char* item) {
          ++restoration_checks;
          if (!ok) {
            ++restoration_failures;
            std::printf("NUMERIC_FENV_RESTORE_FAIL %s host_mode=%u guest_mode=%u "
                        "requested=%08x before=%08x scope=%08x after=%08x flags=%x\n",
                        item, host_mode, guest_mode, requested, before_mxcsr,
                        scope_mxcsr, after_mxcsr, scope_flags);
          }
        };
        verify(before_mxcsr == requested, "real native controls installed");
        verify(scope_flags == 0, "all inherited exception flags isolated");
        verify(scope_mxcsr == (_MM_MASK_MASK | sse_modes[guest_mode]),
               "standard nontrapping SSE environment installed");
        verify(scope_round == modes[guest_mode], "guest rounding installed");
        verify(std::memcmp(&before, &after, sizeof before) == 0,
               "complete caller fenv restored");
        verify(after_mxcsr == before_mxcsr, "every caller MXCSR bit restored");
      }
    }
  }
  std::printf("NUMERIC_FENV_RESTORE_DONE checks=%u failures=%u\n",
              restoration_checks, restoration_failures);
  return restoration_failures;
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
      current_host_mode = host_mode;
      current_guest_mode = guest_mode;
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
  const unsigned restoration_failures = CheckNativeStateRestoration();
  return failures == 0 && restoration_failures == 0 ? 0 : 1;
}
