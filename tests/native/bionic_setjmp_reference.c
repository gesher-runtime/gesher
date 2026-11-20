// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "bionic_setjmp_contracts.h"
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__aarch64__)
extern int Arm64SetjmpRegisters(void);
#endif

// Keep the signal-64 endpoint separate from the common oracle: QEMU linux-user
// may not represent this guest signal even before setjmp runs. Run this mode on
// an actual Android kernel and compare both libc and the raw kernel mask.
static int JumpHighestSignal(void) {
  sigset_t original, requested, observed;
  uint64_t baseline = 0, before = 0, after = 0;
  const uint64_t high_bit = UINT64_C(1) << 63;
  if (SIGRTMAX != 64 || sigprocmask(SIG_SETMASK, NULL, &original) != 0) return 1;
  // Bionic keeps its internal POSIX-timer signal blocked even for an empty
  // public mask. Measure that kernel baseline rather than assuming it is zero.
  int failures = sigemptyset(&requested) != 0 ||
                 sigprocmask(SIG_SETMASK, &requested, NULL) != 0 ||
                 sigprocmask(SIG_SETMASK, NULL, &observed) != 0 ||
                 syscall(__NR_rt_sigprocmask, SIG_SETMASK, NULL, &baseline, sizeof baseline) != 0 ||
                 sigismember(&observed, SIGRTMAX) != 0 || (baseline & high_bit) != 0;
  printf("Android empty-public-mask kernel baseline: %016llx\n",
         (unsigned long long)baseline);
  if (failures) {
    puts("FAIL  could not establish the empty-public-mask kernel baseline");
    (void)sigprocmask(SIG_SETMASK, &original, NULL);
    return 1;
  }
  failures = sigaddset(&requested, SIGRTMAX) != 0 ||
             sigprocmask(SIG_SETMASK, &requested, NULL) != 0 ||
             sigprocmask(SIG_SETMASK, NULL, &observed) != 0 ||
             syscall(__NR_rt_sigprocmask, SIG_SETMASK, NULL, &before, sizeof before) != 0 ||
             sigismember(&observed, SIGRTMAX) != 1 || before != (baseline | high_bit);
  printf("Android signal-64 kernel mask before setjmp: %016llx\n",
         (unsigned long long)before);
  if (failures) {
    puts("FAIL  signal 64 is unavailable before setjmp");
    (void)sigprocmask(SIG_SETMASK, &original, NULL);
    return 1;
  }
  sigjmp_buf env;
  int result = sigsetjmp(env, 1);
  if (result == 0) {
    sigemptyset(&requested);
    if (sigprocmask(SIG_SETMASK, &requested, NULL) != 0) {
      (void)sigprocmask(SIG_SETMASK, &original, NULL);
      return 1;
    }
    siglongjmp(env, 7);
  }
  failures = result != 7 || sigprocmask(SIG_SETMASK, NULL, &observed) != 0 ||
             syscall(__NR_rt_sigprocmask, SIG_SETMASK, NULL, &after, sizeof after) != 0 ||
             sigismember(&observed, SIGRTMAX) != 1 || after != before;
  failures |= sigprocmask(SIG_SETMASK, &original, NULL) != 0;
  printf("Android signal-64 setjmp restoration: %s (kernel mask %016llx)\n",
         failures ? "FAILED" : "passed", (unsigned long long)after);
  return failures;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc == 2 && strcmp(argv[1], "--highest-signal") == 0) return JumpHighestSignal();
  if (argc == 2) {
    int flag = atoi(argv[1]);
    sigjmp_buf env;
    printf("Bionic invalid sigsetjmp flag probe: %d\n", flag);
    (void)sigsetjmp(env, flag);
    puts("FAIL  invalid sigsetjmp flag returned");
    return 2;
  }
  printf("Android Bionic setjmp reference: pointer=%zu, sigset_t=%zu, jmp_buf=%zu, sigjmp_buf=%zu\n",
         sizeof(void*), sizeof(sigset_t), sizeof(jmp_buf), sizeof(sigjmp_buf));
  int failures = BionicSetjmpContracts();
#if defined(__aarch64__)
  int registers = Arm64SetjmpRegisters();
  printf("arm64 setjmp register/FP oracle: %s\n", registers == 0 ? "passed" : "FAILED");
  failures += registers;
#endif
  return failures == 0 ? 0 : 1;
}
