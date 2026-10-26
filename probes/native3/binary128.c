// Verify the compiler's binary128 arithmetic and the MS/System V boundary
// before reusing bionic's long-double libm sources. No CRT long-double calls.
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef long double Quad;
_Static_assert(sizeof(Quad) == 16 && LDBL_MANT_DIG == 113,
               "This probe requires IEEE binary128 long double");

#define NOINLINE __attribute__((noinline))
#define SYSV __attribute__((sysv_abi, noinline))

static NOINLINE Quad add_ms(Quad a, Quad b) { return a + b; }
static NOINLINE Quad sub_ms(Quad a, Quad b) { return a - b; }
static NOINLINE Quad mul_ms(Quad a, Quad b) { return a * b; }
static NOINLINE Quad div_ms(Quad a, Quad b) { return a / b; }
static NOINLINE Quad from_uint_ms(uint64_t value) { return (Quad)value; }
static NOINLINE uint64_t to_uint_ms(Quad value) { return (uint64_t)value; }

static SYSV Quad add_sysv(Quad a, Quad b) { return add_ms(a, b); }
static SYSV Quad sub_sysv(Quad a, Quad b) { return sub_ms(a, b); }
static SYSV Quad mul_sysv(Quad a, Quad b) { return mul_ms(a, b); }
static SYSV Quad div_sysv(Quad a, Quad b) { return div_ms(a, b); }
static SYSV Quad from_uint_sysv(uint64_t value) { return from_uint_ms(value); }
static SYSV uint64_t to_uint_sysv(Quad value) { return to_uint_ms(value); }

static int failures;
static void check(const char* name, int good) {
  printf("%s  native3 binary128: %s\n", good ? "PASS" : "FAIL", name);
  if (!good) ++failures;
}

static int bits(Quad value, uint64_t low, uint64_t high) {
  uint64_t words[2];
  memcpy(words, &value, sizeof(words));
  return words[0] == low && words[1] == high;
}

int main(void) {
  volatile Quad one = 1.0L;
  volatile Quad small = 0x1p-100L;
  volatile Quad two = 2.0L;
  volatile Quad three = 3.0L;
  Quad sum = add_sysv(one, small);
  check("1 + 2^-100 retains bits beyond binary80",
        bits(sum, UINT64_C(0x1000), UINT64_C(0x3fff000000000000)));
  check("MS ABI helper and System V entry agree",
        bits(add_ms(one, small), UINT64_C(0x1000), UINT64_C(0x3fff000000000000)));
  check("subtraction preserves 2^-100",
        bits(sub_sysv(sum, one), 0, UINT64_C(0x3f9b000000000000)));
  check("multiplication rounds in binary128",
        bits(mul_sysv(sum, sum), UINT64_C(0x2000), UINT64_C(0x3fff000000000000)));
  check("division produces binary128 1/3",
        bits(div_sysv(one, three), UINT64_C(0x5555555555555555),
             UINT64_C(0x3ffd555555555555)));
  check("multiply/divide round trip retains low fraction bits",
        bits(div_sysv(mul_sysv(sum, two), two), UINT64_C(0x1000),
             UINT64_C(0x3fff000000000000)));
  volatile uint64_t integer = UINT64_C(0x800000000000007f);
  Quad converted = from_uint_sysv(integer);
  check("uint64 conversion retains every integer bit",
        bits(converted, UINT64_C(0x00fe000000000000), UINT64_C(0x403e000000000000)));
  check("binary128 to uint64 crosses the ABI boundary",
        to_uint_sysv(converted) == integer);
  printf("%d test(s) failed\n", failures);
  return failures != 0;
}
