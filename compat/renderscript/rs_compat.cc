// Gesher: bionic functions libRS calls that mingw's C runtime lacks
// (compat/renderscript/include/gesher_renderscript.h, docs/renderscript.md).

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <windows.h>
// bcrypt.h needs windows.h first.
#include <bcrypt.h>

#include "gesher_renderscript.h"

extern "C" void* memalign(size_t alignment, size_t size) {
  // posix_memalign wants a power of two that is a multiple of sizeof(void*);
  // memalign takes any power of two.
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  void* p = nullptr;
  int error = posix_memalign(&p, alignment, size);
  if (error != 0) {
    errno = error;
    return nullptr;
  }
  return p;
}

extern "C" uint32_t arc4random(void) {
  uint32_t value = 0;
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    abort();  // as bionic does when it cannot get entropy
  }
  return value;
}

extern "C" float lgammaf_r(float x, int* sign) {
  // The sign of Gamma(x): positive for x > 0; for a negative non-integer,
  // (-1)^ceil(-x). At the poles (0 and the negative integers) lgamma is
  // +infinity and the sign is taken as positive, -0 negative, as glibc does.
  int s = 1;
  if (x < 0.0f) {
    float c = ceilf(-x);
    if (c != -x) s = (fmodf(c, 2.0f) == 1.0f) ? -1 : 1;
  } else if (x == 0.0f && signbit(x)) {
    s = -1;
  }
  if (sign != nullptr) *sign = s;
  return lgammaf(x);
}
