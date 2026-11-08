// Independent kernel reference for the exact public ELF fixture.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static void check(const char* what, int ok) {
  printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
  ++checks;
  if (!ok) ++failures;
}
#include "positioned_io_contract_test.inc"
int main(void) {
  test_positioned_io_contracts();
  printf("Positioned Linux reference: %d assertions, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
