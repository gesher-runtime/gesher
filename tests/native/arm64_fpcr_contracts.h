// Actual guest tests shared by native-process and JNI/APK fixtures (ADR0067).
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
int arm64_reference_fesetround(int);
int arm64_reference_fegetround(void);
#ifdef __cplusplus
}
#endif

static uint32_t ArmFpcrRead(void) {
  uint64_t value;
  __asm__ volatile("mrs %0, fpcr" : "=r"(value));
  return (uint32_t)value;
}
static void ArmFpcrWrite(uint32_t value) {
  __asm__ volatile("msr fpcr, %0" : : "r"((uint64_t)value) : "memory");
}
static uint64_t ArmFpcrAdd(int negative) {
  uint64_t one = negative ? UINT64_C(0xbff0000000000000) : UINT64_C(0x3ff0000000000000);
  uint64_t half_ulp = negative ? UINT64_C(0xbca0000000000000) : UINT64_C(0x3ca0000000000000);
  uint64_t result;
  __asm__ volatile("fmov d0, %1\n fmov d1, %2\n fadd d0, d0, d1\n fmov %0, d0"
                   : "=r"(result) : "r"(one), "r"(half_ulp) : "d0", "d1");
  return result;
}
static int64_t ArmFpcrIntegral(int negative) {
  uint64_t value = negative ? UINT64_C(0xbff8000000000000) : UINT64_C(0x3ff8000000000000);
  int64_t result;
  __asm__ volatile("fmov d0, %1\n frinti d0, d0\n fcvtzs %0, d0"
                   : "=r"(result) : "r"(value) : "d0");
  return result;
}
static int ArmFpcrCheck(int condition, const char* what) {
  if (!condition) printf("FAIL arm64 FPCR guest: %s\n", what);
  return condition ? 0 : 1;
}

struct ArmFpcrThreads {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int ready;
};
struct ArmFpcrWorker {
  struct ArmFpcrThreads* gate;
  uint32_t inherited;
  int mode;
  int failures;
};
static const uint64_t ArmFpcrPositive[4] = {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000001),
                                          UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000)};
static const uint64_t ArmFpcrNegative[4] = {UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000000),
                                          UINT64_C(0xbff0000000000001), UINT64_C(0xbff0000000000000)};
static void* ArmFpcrThread(void* opaque) {
  struct ArmFpcrWorker* worker = (struct ArmFpcrWorker*)opaque;
  int failures = ArmFpcrCheck(ArmFpcrRead() == worker->inherited, "pthread_create inherits caller FPCR");
  pthread_mutex_lock(&worker->gate->mutex);
  while (!worker->gate->ready) pthread_cond_wait(&worker->gate->condition, &worker->gate->mutex);
  pthread_mutex_unlock(&worker->gate->mutex);
  failures += ArmFpcrCheck(fesetround(worker->mode) == 0, "worker selects its own mode");
  for (unsigned i = 0; i < 64; ++i) {
    uint32_t expected = (worker->inherited & ~(UINT32_C(3) << 22)) | ((uint32_t)worker->mode << 22);
    failures += ArmFpcrCheck(ArmFpcrRead() == expected && fegetround() == worker->mode &&
                             ArmFpcrAdd(0) == ArmFpcrPositive[worker->mode] &&
                             ArmFpcrAdd(1) == ArmFpcrNegative[worker->mode],
                             "concurrent state and actual arithmetic stay thread-local");
    sched_yield();
  }
  worker->failures = failures;
  return opaque;
}

static uint32_t ArmFpcrCallbackExpected;
static int ArmFpcrCallbackFailures;
static int ArmFpcrCallbackCalls;
static int ArmFpcrInnerCompare(const void* left, const void* right) {
  ArmFpcrCallbackFailures += ArmFpcrCheck(ArmFpcrRead() == ArmFpcrCallbackExpected,
                                         "inner callback inherits actual FPCR");
  int mode = (int)((ArmFpcrCallbackExpected >> 22) & 3);
  mode = (mode + 1) & 3;
  ArmFpcrCallbackFailures += ArmFpcrCheck(arm64_reference_fesetround(mode) == 0,
                                         "pinned callback body writes FPCR by MSR");
  ArmFpcrCallbackExpected = (ArmFpcrCallbackExpected & ~(UINT32_C(3) << 22)) | ((uint32_t)mode << 22);
  ++ArmFpcrCallbackCalls;
  return *(const int*)left - *(const int*)right;
}
static int ArmFpcrOuterCompare(const void* left, const void* right) {
  ArmFpcrCallbackFailures += ArmFpcrCheck(ArmFpcrRead() == ArmFpcrCallbackExpected,
                                         "outer callback inherits actual parent FPCR");
  int values[2] = {2, 1};
  qsort(values, 2, sizeof(int), ArmFpcrInnerCompare);
  ArmFpcrCallbackFailures += ArmFpcrCheck(ArmFpcrRead() == ArmFpcrCallbackExpected &&
                                         fegetround() == (int)((ArmFpcrCallbackExpected >> 22) & 3),
                                         "inner direct MSR persists after host callback return");
  ++ArmFpcrCallbackCalls;
  return *(const int*)left - *(const int*)right;
}

static int Arm64FpcrContracts(void) {
  int failures = 0;
  uint32_t saved = ArmFpcrRead();
  const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  failures += ArmFpcrCheck(modes[0] == 0 && modes[1] == 1 && modes[2] == 2 && modes[3] == 3,
                           "actual Android ARM mode ABI");
  typedef int (*SetRound)(int);
  typedef int (*GetRound)(void);
  SetRound setround = (SetRound)dlsym(RTLD_DEFAULT, "fesetround");
  GetRound getround = (GetRound)dlsym(RTLD_DEFAULT, "fegetround");
  failures += ArmFpcrCheck(setround != NULL && getround != NULL, "both dynamic rounding entries");
  if (setround == NULL || getround == NULL) return failures;
  const int64_t positive_integral[4] = {2, 2, 1, 1};
  const int64_t negative_integral[4] = {-2, -1, -2, -1};
  for (int mode = 0; mode < 4; ++mode) {
    uint32_t seed = UINT32_C(0x03000000) | ((uint32_t)(3 - mode) << 22);
    ArmFpcrWrite(seed);
    failures += ArmFpcrCheck(fesetround(mode) == 0 && getround() == mode && fegetround() == mode,
                             "plain/dynamic entries select and read all four modes");
    uint32_t actual = ArmFpcrRead();
    failures += ArmFpcrCheck(actual == (UINT32_C(0x03000000) | ((uint32_t)mode << 22)),
                             "setter preserves supported non-rounding bits");
    failures += ArmFpcrCheck(ArmFpcrAdd(0) == ArmFpcrPositive[mode] &&
                             ArmFpcrAdd(1) == ArmFpcrNegative[mode] &&
                             ArmFpcrIntegral(0) == positive_integral[mode] &&
                             ArmFpcrIntegral(1) == negative_integral[mode],
                             "FADD and FRINTI obey actual FPCR for both signs");
    ArmFpcrWrite(seed);
    failures += ArmFpcrCheck(arm64_reference_fesetround(mode) == 0 && ArmFpcrRead() == actual &&
                             arm64_reference_fegetround() == mode && getround() == mode,
                             "unchanged pinned bionic body and direct MSR agree");
    failures += ArmFpcrCheck(setround((mode + 1) & 3) == 0 &&
                             arm64_reference_fegetround() == ((mode + 1) & 3),
                             "dynamic setter updates the actual instruction-visible register");
  }
  const int invalid[] = {-1, 4, INT_MIN, INT_MAX};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    uint32_t before = ArmFpcrRead();
    errno = EDOM;
    failures += ArmFpcrCheck(setround(invalid[i]) == -1 && ArmFpcrRead() == before && errno == EDOM,
                             "invalid modes preserve FPCR and errno");
    failures += ArmFpcrCheck(arm64_reference_fesetround(invalid[i]) == -1 && ArmFpcrRead() == before,
                             "pinned invalid-mode behavior agrees");
  }
  uint32_t inherited = UINT32_C(0x03000000) | (UINT32_C(2) << 22);
  ArmFpcrWrite(inherited);
  struct ArmFpcrThreads gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
  struct ArmFpcrWorker workers[4];
  pthread_t threads[4];
  int created[4];
  for (int mode = 0; mode < 4; ++mode) {
    workers[mode].gate = &gate;
    workers[mode].inherited = inherited;
    workers[mode].mode = mode;
    workers[mode].failures = 0;
    created[mode] = pthread_create(&threads[mode], NULL, ArmFpcrThread, &workers[mode]) == 0;
    failures += ArmFpcrCheck(created[mode], "create concurrent guest worker");
  }
  pthread_mutex_lock(&gate.mutex);
  gate.ready = 1;
  pthread_cond_broadcast(&gate.condition);
  pthread_mutex_unlock(&gate.mutex);
  for (int mode = 0; mode < 4; ++mode) if (created[mode]) {
    void* result = NULL;
    failures += ArmFpcrCheck(pthread_join(threads[mode], &result) == 0 && result == &workers[mode],
                             "join actual guest thread");
    failures += workers[mode].failures;
  }
  pthread_cond_destroy(&gate.condition);
  pthread_mutex_destroy(&gate.mutex);
  failures += ArmFpcrCheck(ArmFpcrRead() == inherited, "workers preserve their caller's FPCR");
  ArmFpcrCallbackExpected = inherited;
  ArmFpcrCallbackFailures = ArmFpcrCallbackCalls = 0;
  int values[2] = {2, 1};
  qsort(values, 2, sizeof(int), ArmFpcrOuterCompare);
  failures += ArmFpcrCallbackFailures;
  failures += ArmFpcrCheck(ArmFpcrCallbackCalls >= 2 && values[0] == 1 &&
                           ArmFpcrRead() == ArmFpcrCallbackExpected,
                           "nested qsort guest-host-guest calls retain direct MSR updates");
  ArmFpcrWrite(saved);
  if (failures == 0) puts("PASS arm64 FPCR guest: pinned modes0..3, actual arithmetic, bits, pthread inheritance/isolation and nested callbacks");
  return failures;
}
#endif
