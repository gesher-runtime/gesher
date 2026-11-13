/* Linux reference executable for the shared public Windows/guest fixture. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
static int failed, checked;
static void check(const char* label, int passed) {
  ++checked;
  if (!passed) { ++failed; printf("FAIL %s\n", label); }
}
#include "event_loop_contract_test.inc"
int main(void) {
  test_event_loop_contracts();
  printf("event loop contracts: %d checks, %d failures\n", checked, failed);
  return failed != 0;
}
