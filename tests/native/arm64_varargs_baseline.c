// CI-only valid-call diagnosis for ADR0079. The original Surface logger is
// retained verbatim; the deep call is an additional control, never a fix.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((format(printf, 1, 2), noinline)) static void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("surfacegl: ");
  vprintf(fmt, ap);
  printf("\n");
  fflush(stdout);
  va_end(ap);
}

__attribute__((noinline)) static unsigned with_working_buffer(void) {
  volatile uint8_t scratch[2048];
  for (unsigned i = 0; i < sizeof(scratch); ++i) scratch[i] = (uint8_t)i;
  say("invalidation: abi arm64-v8a");
  unsigned checksum = 0;
  for (unsigned i = 0; i < sizeof(scratch); ++i) checksum += scratch[i];
  return checksum;
}

int gesher_selftest(const char* mode) {
  if (mode[0] == 'd') return with_working_buffer() == 261120 ? 0 : 1;
  say("invalidation: abi arm64-v8a");
  return 0;
}
