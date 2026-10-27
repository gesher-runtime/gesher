// Actual sticky-FPSR tests shared by native ELF and JNI/APK fixtures (ADR0067).
#pragma once
#if defined(__aarch64__)
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
int arm64_reference_feclearexcept(int);
int arm64_reference_fegetexceptflag(fexcept_t*, int);
int arm64_reference_fesetexceptflag(const fexcept_t*, int);
int arm64_reference_feraiseexcept(int);
int arm64_reference_fetestexcept(int);
#ifdef __cplusplus
}
#endif

#define ARM_FPSR_QC UINT32_C(0x08000000)
static uint32_t ArmFpsrRead(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpsr" : "=r"(value));
  return (uint32_t)value;
}
static void ArmFpsrWrite(uint32_t value) {
  __asm__ volatile("msr fpsr, %0" : : "r"((uint64_t)value) : "memory");
}
static uint32_t ArmFpsrControl(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpcr" : "=r"(value));
  return (uint32_t)value;
}
static void ArmFpsrSetControl(uint32_t value) {
  __asm__ volatile("msr fpcr, %0" : : "r"((uint64_t)value) : "memory");
}
static uint64_t ArmFpsrArithmetic(int operation, uint64_t left, uint64_t right) {
  uint64_t result;
  if (operation == 0) {
    __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fdiv d0, d0, d1\n fmov %0, d0"
                     : "=r"(result) : "r"(left), "r"(right) : "d0", "d1");
  } else if (operation == 1) {
    __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fmul d0, d0, d1\n fmov %0, d0"
                     : "=r"(result) : "r"(left), "r"(right) : "d0", "d1");
  } else {
    __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fadd d0, d0, d1\n fmov %0, d0"
                     : "=r"(result) : "r"(left), "r"(right) : "d0", "d1");
  }
  return result;
}
static uint32_t ArmFpsrMultiply32(uint32_t left, uint32_t right) {
  uint32_t result;
  __asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fmul s0, s0, s1\n fmov %w0, s0"
                   : "=r"(result) : "r"(left), "r"(right) : "v0", "v1");
  return result;
}
static void ArmFpsrSaturate(void) {
  __asm__ volatile("movi v0.16b, #127\n movi v1.16b, #1\n sqadd v0.16b, v0.16b, v1.16b"
                   : : : "v0", "v1");
}
static int ArmFpsrCheck(int condition, const char* what) {
  if (!condition) printf("FAIL arm64 FPSR guest: %s\n", what);
  return condition ? 0 : 1;
}
static uint32_t ArmFpsrExceptionBits(unsigned bits) {
  return (bits & 31) | ((bits & 32) << 2);
}

struct ArmFpsrGate { pthread_mutex_t mutex; pthread_cond_t condition; int ready; };
struct ArmFpsrWorker {
  struct ArmFpsrGate* gate;
  uint32_t inherited;
  unsigned index;
  int failures;
};
static void* ArmFpsrThread(void* opaque) {
  struct ArmFpsrWorker* worker = (struct ArmFpsrWorker*)opaque;
  int failures = ArmFpsrCheck(ArmFpsrRead() == worker->inherited,
                             "pthread_create inherits actual caller FPSR");
  pthread_mutex_lock(&worker->gate->mutex);
  while (!worker->gate->ready) pthread_cond_wait(&worker->gate->condition, &worker->gate->mutex);
  pthread_mutex_unlock(&worker->gate->mutex);
  uint32_t own = ARM_FPSR_QC | (UINT32_C(1) << worker->index);
  for (unsigned i = 0; i < 64; ++i) {
    ArmFpsrWrite(own);
    failures += ArmFpsrCheck(fetestexcept(FE_ALL_EXCEPT) == (int)(own & FE_ALL_EXCEPT),
                             "concurrent guest flag reads are thread-local");
    (void)ArmFpsrArithmetic(2, UINT64_C(0x3ff0000000000000), UINT64_C(0x3ca0000000000000));
    failures += ArmFpsrCheck(ArmFpsrRead() == (own | FE_INEXACT),
                             "actual inexact arithmetic preserves the worker's other status");
    failures += ArmFpsrCheck(feclearexcept(FE_INEXACT) == 0 && ArmFpsrRead() == own,
                             "partial clear preserves QC and the worker's flags");
    sched_yield();
  }
  worker->failures = failures;
  return opaque;
}

static uint32_t ArmFpsrCallbackExpected;
static int ArmFpsrCallbackFailures;
static int ArmFpsrCallbackCalls;
static int ArmFpsrInnerCompare(const void* left, const void* right) {
  ArmFpsrCallbackFailures += ArmFpsrCheck(ArmFpsrRead() == ArmFpsrCallbackExpected,
                                         "third guest level inherits actual FPSR");
  ArmFpsrCallbackFailures += ArmFpsrCheck(arm64_reference_feclearexcept(FE_ALL_EXCEPT) == 0 &&
                                         arm64_reference_feraiseexcept(FE_DENORMAL | FE_INEXACT) == 0,
                                         "unchanged pinned callback bodies mutate status");
  ArmFpsrCallbackExpected = (ArmFpsrCallbackExpected & ~FE_ALL_EXCEPT) | FE_DENORMAL | FE_INEXACT;
  ++ArmFpsrCallbackCalls;
  return *(const int*)left - *(const int*)right;
}
static int ArmFpsrOuterCompare(const void* left, const void* right) {
  ArmFpsrCallbackFailures += ArmFpsrCheck(ArmFpsrRead() == ArmFpsrCallbackExpected,
                                         "outer callback inherits actual parent FPSR");
  ArmFpsrCallbackExpected ^= ARM_FPSR_QC;
  ArmFpsrWrite(ArmFpsrCallbackExpected);
  int values[2] = {2, 1};
  qsort(values, 2, sizeof(int), ArmFpsrInnerCompare);
  ArmFpsrCallbackFailures += ArmFpsrCheck(ArmFpsrRead() == ArmFpsrCallbackExpected &&
                                         fetestexcept(FE_ALL_EXCEPT) == (int)(ArmFpsrCallbackExpected & FE_ALL_EXCEPT),
                                         "inner direct MSR persists through host callback return");
  ++ArmFpsrCallbackCalls;
  return *(const int*)left - *(const int*)right;
}

static int Arm64FpsrContracts(void) {
  int failures = 0;
  uint32_t saved_control = ArmFpsrControl(), saved_status = ArmFpsrRead();
  failures += ArmFpsrCheck(sizeof(fexcept_t) == 4 && FE_ALL_EXCEPT == 0x9f && FE_DENORMAL == 0x80,
                           "actual Android ARM exception ABI includes denormal");
  typedef int (*One)(int);
  typedef int (*Get)(fexcept_t*, int);
  typedef int (*Set)(const fexcept_t*, int);
  One clear = (One)dlsym(RTLD_DEFAULT, "feclearexcept");
  One raise = (One)dlsym(RTLD_DEFAULT, "feraiseexcept");
  One test = (One)dlsym(RTLD_DEFAULT, "fetestexcept");
  Get getflag = (Get)dlsym(RTLD_DEFAULT, "fegetexceptflag");
  Set setflag = (Set)dlsym(RTLD_DEFAULT, "fesetexceptflag");
  failures += ArmFpsrCheck(clear && raise && test && getflag && setflag, "all five dynamic flag entries");
  if (!clear || !raise || !test || !getflag || !setflag) return failures;
  ArmFpsrSetControl(UINT32_C(0x02000000));
  for (unsigned i = 0; i < 68; ++i) {
    const int special[] = {-1, INT_MIN, INT_MAX, 0x08000000};
    int excepts = i < 64 ? (int)(ArmFpsrExceptionBits(i) | UINT32_C(0x48000000)) : special[i - 64];
    uint32_t before = ARM_FPSR_QC | ArmFpsrExceptionBits((i * 13) & 63);
    ArmFpsrWrite(before);
    int expected_result = arm64_reference_feclearexcept(excepts);
    uint32_t expected_status = ArmFpsrRead();
    ArmFpsrWrite(before);
    errno = EDOM;
    failures += ArmFpsrCheck(clear(excepts) == expected_result && ArmFpsrRead() == expected_status && errno == EDOM,
                             "clear masks and state match the original body");
    ArmFpsrWrite(before);
    expected_result = arm64_reference_feraiseexcept(excepts);
    expected_status = ArmFpsrRead();
    ArmFpsrWrite(before);
    errno = EDOM;
    failures += ArmFpsrCheck(raise(excepts) == expected_result && ArmFpsrRead() == expected_status && errno == EDOM,
                             "raise sets only pinned status bits without changing errno");
    struct { uint32_t before; fexcept_t flag; uint32_t after; } observed = {0x12345678, UINT32_MAX, 0x87654321};
    fexcept_t expected_flag = 0;
    ArmFpsrWrite(before);
    expected_result = arm64_reference_fegetexceptflag(&expected_flag, excepts);
    errno = EDOM;
    failures += ArmFpsrCheck(getflag(&observed.flag, excepts) == expected_result && observed.flag == expected_flag &&
                             observed.before == 0x12345678 && observed.after == 0x87654321 &&
                             ArmFpsrRead() == before && errno == EDOM,
                             "getflag writes exactly four bytes and preserves status/errno");
    fexcept_t replacement = ARM_FPSR_QC | ArmFpsrExceptionBits((i * 7 + 9) & 63);
    expected_result = arm64_reference_fesetexceptflag(&replacement, excepts);
    expected_status = ArmFpsrRead();
    ArmFpsrWrite(before);
    errno = EDOM;
    failures += ArmFpsrCheck(setflag(&replacement, excepts) == expected_result && ArmFpsrRead() == expected_status &&
                             replacement == (ARM_FPSR_QC | ArmFpsrExceptionBits((i * 7 + 9) & 63)) && errno == EDOM,
                             "setflag masks match the original body and preserve the source");
    failures += ArmFpsrCheck(test(excepts) == arm64_reference_fetestexcept(excepts) &&
                             fetestexcept(excepts) == test(excepts) &&
                             ArmFpsrControl() == UINT32_C(0x02000000),
                             "plain/dynamic query agrees with original MRS and preserves FPCR");
  }
  ArmFpsrWrite(ARM_FPSR_QC);
  fexcept_t plain_flag = 0;
  failures += ArmFpsrCheck(feraiseexcept(FE_DENORMAL | FE_INVALID) == 0 &&
                           fegetexceptflag(&plain_flag, -1) == 0 && plain_flag == (FE_DENORMAL | FE_INVALID),
                           "plain raise/getflag imports reach the guest definitions");
  plain_flag = FE_DIVBYZERO;
  failures += ArmFpsrCheck(fesetexceptflag(&plain_flag, -1) == 0 &&
                           ArmFpsrRead() == (ARM_FPSR_QC | FE_DIVBYZERO) &&
                           feclearexcept(FE_DIVBYZERO) == 0 && fetestexcept(-1) == 0 &&
                           ArmFpsrRead() == ARM_FPSR_QC,
                           "plain setflag/clear/query preserve non-exception status");
  struct Operation { int operation; uint64_t left, right; uint32_t control, flags; } operations[] = {
      {0, UINT64_C(0x3ff0000000000000), 0, 0, FE_DIVBYZERO},
      {0, 0, 0, 0, FE_INVALID},
      {1, UINT64_C(0x7fefffffffffffff), UINT64_C(0x4000000000000000), 0, FE_OVERFLOW | FE_INEXACT},
      {1, UINT64_C(0x0010000000000000), UINT64_C(0x0010000000000000), 0, FE_UNDERFLOW | FE_INEXACT},
      {2, UINT64_C(0x3ff0000000000000), UINT64_C(0x3ca0000000000000), 0, FE_INEXACT},
      {2, 1, UINT64_C(0x3ff0000000000000), UINT32_C(1) << 24, FE_DENORMAL}};
  for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
    struct Operation op = operations[i];
    ArmFpsrSetControl(op.control);
    ArmFpsrWrite(0);
    (void)ArmFpsrArithmetic(op.operation, op.left, op.right);
    failures += ArmFpsrCheck(ArmFpsrRead() == op.flags && test(FE_ALL_EXCEPT) == (int)op.flags &&
                             arm64_reference_fetestexcept(FE_ALL_EXCEPT) == (int)op.flags,
                             "real FDIV/FMUL/FADD produces each pinned exception flag");
  }
  const uint32_t seeds[] = {0, ARM_FPSR_QC | FE_INVALID | FE_DENORMAL,
                            ARM_FPSR_QC | FE_INVALID | FE_DENORMAL | FE_INEXACT};
  for (unsigned mode = 0; mode < 4; ++mode) for (unsigned dn = 0; dn < 2; ++dn) {
    uint32_t control = (mode << 22) | (dn << 25) | (UINT32_C(1) << 24);
    for (unsigned width = 32; width <= 64; width += 32) for (unsigned initial = 0; initial < 3; ++initial) {
      ArmFpsrSetControl(control);
      ArmFpsrWrite(seeds[initial]);
      uint64_t result = width == 32 ? ArmFpsrMultiply32(UINT32_C(0x00800000), UINT32_C(0x00800000)) :
          ArmFpsrArithmetic(1, UINT64_C(0x0010000000000000), UINT64_C(0x0010000000000000));
      uint32_t expected = seeds[initial] | FE_UNDERFLOW;
      failures += ArmFpsrCheck(result == 0 && ArmFpsrRead() == expected &&
                               test(FE_ALL_EXCEPT) == (int)(expected & FE_ALL_EXCEPT) &&
                               ArmFpsrControl() == control,
                               "FZ32/64 output underflow preserves prior IXC without adding IXC");
    }
  }
  ArmFpsrSetControl(0);
  ArmFpsrWrite(0);
  ArmFpsrSaturate();
  failures += ArmFpsrCheck(ArmFpsrRead() == ARM_FPSR_QC && test(-1) == 0 &&
                           feclearexcept(-1) == 0 && ArmFpsrRead() == ARM_FPSR_QC,
                           "actual saturating SIMD sets QC outside the exception mask");
  uint32_t inherited = ARM_FPSR_QC | FE_INVALID | FE_DENORMAL;
  ArmFpsrWrite(inherited);
  struct ArmFpsrGate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
  struct ArmFpsrWorker workers[4];
  pthread_t threads[4];
  int created[4];
  for (unsigned i = 0; i < 4; ++i) {
    workers[i].gate = &gate; workers[i].inherited = inherited; workers[i].index = i; workers[i].failures = 0;
    created[i] = pthread_create(&threads[i], NULL, ArmFpsrThread, &workers[i]) == 0;
    failures += ArmFpsrCheck(created[i], "create concurrent guest status worker");
  }
  pthread_mutex_lock(&gate.mutex);
  gate.ready = 1;
  pthread_cond_broadcast(&gate.condition);
  pthread_mutex_unlock(&gate.mutex);
  for (unsigned i = 0; i < 4; ++i) if (created[i]) {
    void* result = NULL;
    failures += ArmFpsrCheck(pthread_join(threads[i], &result) == 0 && result == &workers[i], "join guest status worker");
    failures += workers[i].failures;
  }
  pthread_cond_destroy(&gate.condition);
  pthread_mutex_destroy(&gate.mutex);
  failures += ArmFpsrCheck(ArmFpsrRead() == inherited, "workers preserve caller status");
  ArmFpsrCallbackExpected = inherited;
  ArmFpsrCallbackFailures = ArmFpsrCallbackCalls = 0;
  int values[2] = {2, 1};
  qsort(values, 2, sizeof(int), ArmFpsrOuterCompare);
  failures += ArmFpsrCallbackFailures;
  failures += ArmFpsrCheck(ArmFpsrCallbackCalls >= 2 && values[0] == 1 && ArmFpsrRead() == ArmFpsrCallbackExpected,
                           "nested callbacks persist direct and pinned-body FPSR changes");
  ArmFpsrSetControl(saved_control);
  ArmFpsrWrite(saved_status);
  if (failures == 0) puts("PASS arm64 FPSR guest: pinned flags0x9f, masks, real arithmetic, QC, pthread inheritance/isolation and nested callbacks");
  return failures;
}
#undef ARM_FPSR_QC
#endif
