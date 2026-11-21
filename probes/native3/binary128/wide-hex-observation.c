// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fenv.h>
#include <inttypes.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int main(void) {
  const char text[] = "0x1.23456789abcdep12";
  const wchar_t wide[] = L"0x1.23456789abcdep12";
  const int modes[] = {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
  locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", NULL);
  if (!locale || sizeof(long double) != 16) return 2;
  for (unsigned mode = 0; mode < 4; ++mode) {
    for (unsigned api = 0; api < 4; ++api) {
      if (fesetround(modes[mode]) || feclearexcept(FE_ALL_EXCEPT)) return 3;
      errno = 123;
      char* end = NULL;
      wchar_t* wend = NULL;
      long double value;
      switch (api) {
        case 0: value = strtold(text, &end); break;
        case 1: value = strtold_l(text, &end, locale); break;
        case 2: value = wcstold(wide, &wend); break;
        default: value = wcstold_l(wide, &wend, locale); break;
      }
      int error = errno;
      uint64_t bits[2];
      memcpy(bits, &value, sizeof(bits));
      printf("WIDE_HEX %u %u %016" PRIx64 "%016" PRIx64 " %td %d %d\n",
             mode, api, bits[1], bits[0], api < 2 ? end - text : wend - wide,
             error, fegetround() == modes[mode]);
    }
  }
  freelocale(locale);
  puts("WIDE_HEX_DONE");
  return 0;
}
