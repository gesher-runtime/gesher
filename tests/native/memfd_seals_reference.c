// The same mapping-lease assertions used by the Android guest fixture, on Linux.
// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failures;
static void check(const char* name, int passed) {
  ++checks;
  if (!passed) { ++failures; fprintf(stderr, "FAIL %s (errno=%d)\n", name, errno); }
}
#include "memfd_contract_test.inc"

int main(void) {
  test_memfd_mapping_leases(memfd_from_syscall);
  printf("Linux memfd mapping-lease reference: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
