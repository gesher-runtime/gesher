// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Controls distinguish observable emulator arithmetic flags from parser flags.
#pragma STDC FENV_ACCESS ON
#include <errno.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int flags(void) {
  int f = fetestexcept(FE_ALL_EXCEPT);
  return ((f & FE_INVALID) ? 1 : 0) | ((f & FE_DIVBYZERO) ? 2 : 0) |
         ((f & FE_OVERFLOW) ? 4 : 0) | ((f & FE_UNDERFLOW) ? 8 : 0) |
         ((f & FE_INEXACT) ? 16 : 0);
}

int main(void) {
  fenv_t saved;
  if (fegetenv(&saved)) return 2;
  const int modes[] = {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
  for (unsigned m = 0; m < 4; ++m) {
    if (fesetround(modes[m])) return 3;
    volatile double one = 1.0, ten = 10.0;
    feclearexcept(FE_ALL_EXCEPT);
    volatile double quotient = one / ten;
    int division_flags = flags();
    uint64_t division_bits;
    double copied = quotient;
    memcpy(&division_bits, &copied, sizeof(copied));
    printf("FENV arithmetic %u %d %016llx\n", m, division_flags, (unsigned long long)division_bits);
    feclearexcept(FE_ALL_EXCEPT);
    int raised = feraiseexcept(FE_DIVBYZERO | FE_INEXACT);
    printf("FENV explicit %u %d %d\n", m, raised, flags());
    for (unsigned sticky = 0; sticky < 2; ++sticky) {
      char* end;
      feclearexcept(FE_ALL_EXCEPT);
      if (sticky) feraiseexcept(FE_DIVBYZERO);
      errno = 123;
      volatile double narrow = strtod("0.1", &end);
      int narrow_error = errno;
      int narrow_flags = flags();
      (void)narrow;
      printf("FENV strtod %u %u %d %d\n", m, sticky, narrow_error, narrow_flags);
      feclearexcept(FE_ALL_EXCEPT);
      if (sticky) feraiseexcept(FE_DIVBYZERO);
      errno = 123;
      volatile long double wide = strtold("0.1", &end);
      int wide_error = errno;
      int wide_flags = flags();
      (void)wide;
      printf("FENV strtold %u %u %d %d\n", m, sticky, wide_error, wide_flags);
    }
  }
  if (fesetenv(&saved)) return 4;
  puts("FENV_REFERENCE_DONE");
  return 0;
}
