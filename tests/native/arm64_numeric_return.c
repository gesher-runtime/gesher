// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Ordinary public libc calls, built by the NDK as an executable or shared
// library. Every quad input is exactly representable as a normal binary64,
// so these return-bit checks do not depend on the new binary128 parser.
#define _GNU_SOURCE
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

_Static_assert(sizeof(long double) == 16 && LDBL_MANT_DIG == 113,
               "Android LP64 binary128 is required");
_Static_assert(sizeof(wchar_t) == 4, "Android UTF32 wchar_t is required");

struct Expected { uint64_t low, high; size_t consumed; };
struct Sample { const char* text; struct Expected narrow, wide; };
// Android API36 observed wide parsing stops at the b in the mixed-digit hex
// sample: the pinned wcstod candidate alphabet omits b/c/d. Preserve both
// API contracts. Observation payload SHA256 af8fc833123b02135b1f3f96bc0cbef0
// 7d3732f497e50195a87c84a540a3959c; initial 32-failure receipt is retained.
static const struct Sample samples[] = {
  {"-0", {0, UINT64_C(0x8000000000000000), 2}, {0, UINT64_C(0x8000000000000000), 2}},
  {"1", {0, UINT64_C(0x3fff000000000000), 1}, {0, UINT64_C(0x3fff000000000000), 1}},
  {"1.5", {0, UINT64_C(0x3fff800000000000), 3}, {0, UINT64_C(0x3fff800000000000), 3}},
  {"-13.25", {0, UINT64_C(0xc002a80000000000), 6}, {0, UINT64_C(0xc002a80000000000), 6}},
  {"0x1.0000000000001p0", {UINT64_C(0x1000000000000000), UINT64_C(0x3fff000000000000), 19}, {UINT64_C(0x1000000000000000), UINT64_C(0x3fff000000000000), 19}},
  {"0x1.23456789abcdep12", {UINT64_C(0xe000000000000000), UINT64_C(0x400b23456789abcd), 20}, {0, UINT64_C(0x3fff23456789a000), 13}},
  {"0x1p-1022", {0, UINT64_C(0x3c01000000000000), 9}, {0, UINT64_C(0x3c01000000000000), 9}},
  {"0x1.fffffffffffffp1023", {UINT64_C(0xf000000000000000), UINT64_C(0x43feffffffffffff), 22}, {UINT64_C(0xf000000000000000), UINT64_C(0x43feffffffffffff), 22}},
};

static unsigned checks, failures;
static void check(int ok, const char* item, unsigned sample, int mode, int api, int seed) {
  ++checks;
  if (!ok) {
    ++failures;
    printf("NUMERIC_RETURN_FAIL %s sample=%u mode=%d api=%d seed=%d\n",
           item, sample, mode, api, seed);
  }
}

int gesher_numeric_selftest(void) {
  checks = failures = 0;
  fenv_t saved;
  if (fegetenv(&saved) != 0 || fesetenv(FE_DFL_ENV) != 0) return 2;
  const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", NULL);
  if (locale == NULL) { fesetenv(&saved); return 3; }
  unsigned conversions = 0;
  for (unsigned c = 0; c < sizeof(samples) / sizeof(samples[0]); ++c) {
    const struct Sample* sample = &samples[c];
    size_t length = strlen(sample->text);
    wchar_t wide[64];
    for (size_t i = 0; i <= length; ++i) wide[i] = (unsigned char)sample->text[i];
    for (int m = 0; m < 4; ++m) {
      for (int api = 0; api < 4; ++api) {
        for (int seed = 0; seed < 2; ++seed) {
          const int prior = seed ? FE_ALL_EXCEPT : 0;
          if (fesetround(modes[m]) != 0 || feclearexcept(FE_ALL_EXCEPT) != 0 ||
              (prior && feraiseexcept(prior) != 0)) {
            freelocale(locale); fesetenv(&saved); return 4;
          }
          errno = 123;
          char* end = NULL;
          wchar_t* wide_end = NULL;
          long double result;
          switch (api) {
            case 0: result = strtold(sample->text, &end); break;
            case 1: result = strtold_l(sample->text, &end, locale); break;
            case 2: result = wcstold(wide, &wide_end); break;
            default: result = wcstold_l(wide, &wide_end, locale); break;
          }
          const int error = errno;
          const int flags = fetestexcept(FE_ALL_EXCEPT);
          const int round = fegetround();
          uint64_t bits[2];
          memcpy(bits, &result, sizeof bits);
          const struct Expected* expected = api < 2 ? &sample->narrow : &sample->wide;
          check(bits[0] == expected->low && bits[1] == expected->high,
                "full binary128 result", c, m, api, seed);
          check(api < 2 ? end == sample->text + expected->consumed :
                          wide_end == wide + expected->consumed,
                "end pointer", c, m, api, seed);
          check(error == 123, "errno preserved", c, m, api, seed);
          check(round == modes[m], "guest rounding preserved", c, m, api, seed);
          check((flags & prior) == prior, "prior guest flags preserved", c, m, api, seed);
          ++conversions;
        }
      }
    }
  }
  // The new high XMM0 word must leave all scalar return channels intact.
  for (int m = 0; m < 4; ++m) {
    if (fesetround(modes[m]) != 0) { freelocale(locale); fesetenv(&saved); return 5; }
    char* end = NULL;
    float single = strtof("-13.25", &end);
    uint32_t single_bits;
    memcpy(&single_bits, &single, sizeof single_bits);
    check(single_bits == UINT32_C(0xc1540000) && *end == '\0', "float result", 0, m, 4, 0);
    double real = strtod("0x1.23456789abcdep12", &end);
    uint64_t double_bits;
    memcpy(&double_bits, &real, sizeof double_bits);
    check(double_bits == UINT64_C(0x40b23456789abcde) && *end == '\0', "double result", 0, m, 5, 0);
    unsigned long long integer = strtoull("123456789abcdef0", &end, 16);
    check(integer == UINT64_C(0x123456789abcdef0) && *end == '\0', "integer result", 0, m, 6, 0);
  }
  freelocale(locale);
  if (fesetenv(&saved) != 0) return 6;
  printf("NUMERIC_RETURN_DONE conversions=%u checks=%u failures=%u\n",
         conversions, checks, failures);
  return failures == 0 ? 0 : 1;
}

#ifndef GESHER_NUMERIC_SHARED
int main(void) { return gesher_numeric_selftest(); }
#endif
