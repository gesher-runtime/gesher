// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Ordinary Android libc calls. This records observations; it does not use
// Gesher's parser, floating-point formatter or a host long-double conversion.
#define _GNU_SOURCE
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../../tests/binary128/parse_cases.h"

_Static_assert(sizeof(long double) == 16 && LDBL_MANT_DIG == 113,
               "Android LP64 binary128 is required");
_Static_assert(sizeof(wchar_t) == 4, "Android UTF32 wchar_t is required");

static int flags(void) {
  int f = fetestexcept(FE_ALL_EXCEPT);
  return ((f & FE_INVALID) ? 1 : 0) | ((f & FE_DIVBYZERO) ? 2 : 0) |
         ((f & FE_OVERFLOW) ? 4 : 0) | ((f & FE_UNDERFLOW) ? 8 : 0) |
         ((f & FE_INEXACT) ? 16 : 0);
}

int main(void) {
  const int modes[] = {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
  locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", NULL);
  if (locale == NULL) return 2;
  for (unsigned c = 0; c < sizeof(quad_cases) / sizeof(quad_cases[0]); ++c) {
    const struct QuadCase* sample = &quad_cases[c];
    wchar_t wide[4096];
    size_t n = strlen(sample->text);
    if (n + 2 >= sizeof(wide) / sizeof(wide[0])) return 3;
    size_t start = sample->wide_space ? 1 : 0;
    if (start) wide[0] = 0x2003;
    for (size_t i = 0; i <= n; ++i) wide[i + start] = (unsigned char)sample->text[i];
    for (int mode = 0; mode < 4; ++mode) {
      for (int api = 0; api < 4; ++api) {
        if (fesetround(modes[mode]) != 0 || feclearexcept(FE_ALL_EXCEPT) != 0) return 4;
        errno = 123;
        char* end = NULL;
        wchar_t* wend = NULL;
        long double value;
        switch (api) {
          case 0: value = strtold(sample->text, &end); break;
          case 1: value = strtold_l(sample->text, &end, locale); break;
          case 2: value = wcstold(wide, &wend); break;
          default: value = wcstold_l(wide, &wend, locale); break;
        }
        int error = errno;
        int exceptions = flags();
        int unchanged = fegetround() == modes[mode];
        uint64_t bits[2];
        memcpy(bits, &value, sizeof(bits));
        ptrdiff_t consumed = api < 2 ? end - sample->text : wend - wide;
        printf("QUAD %u %d %d %016" PRIx64 "%016" PRIx64 " %td %d %d %d\n",
               c, mode, api, bits[1], bits[0], consumed, error, exceptions, unchanged);
      }
    }
  }
  fesetround(FE_TONEAREST);
  freelocale(locale);
  puts("QUAD_REFERENCE_DONE");
  return 0;
}
