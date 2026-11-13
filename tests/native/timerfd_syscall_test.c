// Standalone ELF driver for the same timerfd numeric-syscall checks used by libc_test.
#include <stdio.h>
static int failures, assertions;
static void check(const char* name, int success) {
    ++assertions;
    printf("timerfd-syscall: %s %s\n", success ? "PASS" : "FAIL", name);
    if (!success) failures++;
}
#include "timerfd_syscall_test.inc"
int gesher_selftest(void) {
    test_timerfd_syscalls();
    printf("timerfd-syscall: %d assertions, %d failures\n", assertions, failures);
    return failures;
}
#if defined(GESHER_TIMERFD_REFERENCE_MAIN)
int main(void) { return gesher_selftest() ? 1 : 0; }
#endif
