// Standalone ELF driver for the same timerfd numeric-syscall checks used by libc_test.
#include <stdio.h>
static int failures;
static void check(const char* name, int success) {
    printf("timerfd-syscall: %s %s\n", success ? "PASS" : "FAIL", name);
    if (!success) failures++;
}
#include "timerfd_syscall_test.inc"
int gesher_selftest(void) { test_timerfd_syscalls(); return failures; }
