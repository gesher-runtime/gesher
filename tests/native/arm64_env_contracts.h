// Actual ARM LP64 fenv contracts against the unchanged pinned bionic body.
#pragma once
#if defined(__aarch64__)
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
extern const fenv_t arm64_reference_default;
int arm64_reference_fegetenv(fenv_t*);
int arm64_reference_fesetenv(const fenv_t*);
int arm64_reference_feholdexcept(fenv_t*);
int arm64_reference_feupdateenv(const fenv_t*);
#ifdef __cplusplus
}
#endif

static int ArmEnvCheck(int condition, const char* name) {
  if (!condition) printf("FAIL arm64 environment guest: %s\n", name);
  return condition ? 0 : 1;
}
static fenv_t ArmEnvRead(void) {
  uint64_t control, status;
  __asm__ volatile("mrs %0, fpcr\n mrs %1, fpsr" : "=r"(control), "=r"(status));
  fenv_t result = {(uint32_t)control, (uint32_t)status};
  return result;
}
static void ArmEnvWrite(const fenv_t* env) {
  __asm__ volatile("msr fpcr, %0\n msr fpsr, %1"
                   : : "r"((uint64_t)env->__control), "r"((uint64_t)env->__status) : "memory");
}
static int ArmEnvEqual(const fenv_t* left, const fenv_t* right) {
  return memcmp(left, right, sizeof(fenv_t)) == 0;
}
struct ArmEnvGuard { uint32_t before; fenv_t env; uint32_t after; };
typedef int (*ArmEnvGet)(fenv_t*);
typedef int (*ArmEnvSet)(const fenv_t*);

static int ArmEnvSnapshot(ArmEnvGet actual, ArmEnvGet reference,
                          const fenv_t* seed, int hold) {
  struct ArmEnvGuard got = {UINT32_C(0x12345678), {0, 0}, UINT32_C(0x87654321)};
  struct ArmEnvGuard want = got;
  ArmEnvWrite(seed);
  errno = EDOM;
  int code = actual(&got.env);
  int saved_errno = errno;
  fenv_t state = ArmEnvRead();
  ArmEnvWrite(seed);
  int ref_code = reference(&want.env);
  fenv_t ref_state = ArmEnvRead();
  return ArmEnvCheck(code == ref_code && code == 0 && saved_errno == EDOM &&
    memcmp(&got, &want, sizeof(got)) == 0 && got.before == UINT32_C(0x12345678) &&
    got.after == UINT32_C(0x87654321) && ArmEnvEqual(&got.env, seed) &&
    ArmEnvEqual(&state, &ref_state) && state.__control == seed->__control &&
    state.__status == (hold ? (seed->__status & ~UINT32_C(0x9f)) : seed->__status),
    "get/hold match pinned body, eight-byte canaries, errno, QC and control");
}
static int ArmEnvInstall(ArmEnvSet actual, ArmEnvSet reference,
                         const fenv_t* seed, const fenv_t* target, int update) {
  fenv_t input = *target;
  ArmEnvWrite(seed);
  errno = EDOM;
  int code = actual(&input);
  int saved_errno = errno;
  fenv_t state = ArmEnvRead();
  ArmEnvWrite(seed);
  int ref_code = reference(target);
  fenv_t ref_state = ArmEnvRead();
  return ArmEnvCheck(code == ref_code && code == 0 && saved_errno == EDOM &&
    ArmEnvEqual(&input, target) && ArmEnvEqual(&state, &ref_state) &&
    state.__control == target->__control &&
    state.__status == (target->__status | (update ? (seed->__status & UINT32_C(0x9f)) : 0)),
    "set/update match pinned body, immutable input, errno and exception merge");
}

struct ArmEnvWorker { fenv_t inherited; unsigned index; int failures; };
static void* ArmEnvThread(void* opaque) {
  struct ArmEnvWorker* worker = (struct ArmEnvWorker*)opaque;
  fenv_t inherited = ArmEnvRead();
  int failures = ArmEnvCheck(ArmEnvEqual(&inherited, &worker->inherited),
                             "pthread inherits both actual environment registers");
  fenv_t own = {UINT32_C(0x03000000) | (worker->index << 22),
                UINT32_C(0x08000000) | (UINT32_C(1) << worker->index)};
  for (unsigned i = 0; i < 16; ++i) {
    failures += ArmEnvInstall(fesetenv, arm64_reference_fesetenv, &inherited, &own, 0);
    sched_yield();
    fenv_t state = ArmEnvRead();
    failures += ArmEnvCheck(ArmEnvEqual(&state, &own), "concurrent environments remain thread-local");
    failures += ArmEnvSnapshot(feholdexcept, arm64_reference_feholdexcept, &own, 1);
    failures += ArmEnvInstall(feupdateenv, arm64_reference_feupdateenv, &own, &inherited, 1);
  }
  worker->failures = failures;
  return opaque;
}

static fenv_t ArmEnvCallbackExpected;
static int ArmEnvCallbackFailures;
static unsigned ArmEnvCallbackCalls;
static int ArmEnvInnerCompare(const void* left, const void* right) {
  fenv_t inherited = ArmEnvRead();
  ArmEnvCallbackFailures += ArmEnvCheck(ArmEnvEqual(&inherited, &ArmEnvCallbackExpected),
                                        "inner callback inherits both registers");
  fenv_t target = {UINT32_C(0x03400000), UINT32_C(0x08000082)};
  arm64_reference_feupdateenv(&target);
  ArmEnvCallbackExpected = ArmEnvRead();
  ArmEnvWrite(&inherited);
  ArmEnvCallbackFailures += ArmEnvCheck(feupdateenv(&target) == 0,
                                        "inner callback updates complete environment");
  fenv_t state = ArmEnvRead();
  ArmEnvCallbackFailures += ArmEnvCheck(ArmEnvEqual(&state, &ArmEnvCallbackExpected),
                                        "inner callback agrees with actual pinned register writes");
  ++ArmEnvCallbackCalls;
  return *(const int*)left - *(const int*)right;
}
static int ArmEnvOuterCompare(const void* left, const void* right) {
  fenv_t inherited = ArmEnvRead();
  ArmEnvCallbackFailures += ArmEnvCheck(ArmEnvEqual(&inherited, &ArmEnvCallbackExpected),
                                        "outer callback inherits complete environment");
  int values[2] = {2, 1};
  qsort(values, 2, sizeof(int), ArmEnvInnerCompare);
  fenv_t state = ArmEnvRead();
  ArmEnvCallbackFailures += ArmEnvCheck(ArmEnvEqual(&state, &ArmEnvCallbackExpected),
                                        "nested update persists through host callback return");
  ++ArmEnvCallbackCalls;
  return *(const int*)left - *(const int*)right;
}

static int Arm64EnvContracts(void) {
  int failures = 0;
  fenv_t saved = ArmEnvRead();
  failures += ArmEnvCheck(sizeof(fenv_t) == 8 && sizeof(fexcept_t) == 4 && FE_ALL_EXCEPT == 0x9f,
                           "actual pinned ARM LP64 environment ABI");
  const fenv_t* dynamic_default = (const fenv_t*)dlsym(RTLD_DEFAULT, "__fe_dfl_env");
  failures += ArmEnvCheck(dynamic_default != NULL && dynamic_default == FE_DFL_ENV &&
    ArmEnvEqual(dynamic_default, &arm64_reference_default) &&
    dynamic_default->__control == 0 && dynamic_default->__status == 0,
    "imported and dynamic default object identity, eight-byte pinned bytes");
  ArmEnvGet gets[2] = {fegetenv, (ArmEnvGet)dlsym(RTLD_DEFAULT, "fegetenv")};
  ArmEnvGet holds[2] = {feholdexcept, (ArmEnvGet)dlsym(RTLD_DEFAULT, "feholdexcept")};
  ArmEnvSet sets[2] = {fesetenv, (ArmEnvSet)dlsym(RTLD_DEFAULT, "fesetenv")};
  ArmEnvSet updates[2] = {feupdateenv, (ArmEnvSet)dlsym(RTLD_DEFAULT, "feupdateenv")};
  if (!dynamic_default || !gets[1] || !holds[1] || !sets[1] || !updates[1]) {
    ArmEnvWrite(&saved);
    return failures + ArmEnvCheck(0, "all dynamic environment entries resolve");
  }
  const uint32_t flags[] = {0, 1, 2, 4, 8, 16, 128, 159};
  unsigned rows = 0;
  for (unsigned mode = 0; mode < 4; ++mode) for (unsigned bits = 0; bits < 4; ++bits) {
    for (unsigned flag = 0; flag < sizeof(flags) / sizeof(flags[0]); ++flag) for (unsigned qc = 0; qc < 2; ++qc) {
      fenv_t seed = {(mode << 22) | (bits << 24), flags[flag] | (qc << 27)};
      fenv_t target = {((3 - mode) << 22) | ((3 - bits) << 24),
                       flags[(flag + 3) % 8] | ((1 - qc) << 27)};
      for (unsigned entry = 0; entry < 2; ++entry) {
        failures += ArmEnvSnapshot(gets[entry], arm64_reference_fegetenv, &seed, 0);
        failures += ArmEnvSnapshot(holds[entry], arm64_reference_feholdexcept, &seed, 1);
        failures += ArmEnvInstall(sets[entry], arm64_reference_fesetenv, &seed, &target, 0);
        failures += ArmEnvInstall(updates[entry], arm64_reference_feupdateenv, &seed, &target, 1);
        failures += ArmEnvInstall(sets[entry], arm64_reference_fesetenv, &seed, FE_DFL_ENV, 0);
        failures += ArmEnvInstall(updates[entry], arm64_reference_feupdateenv, &seed, dynamic_default, 1);
      }
      ++rows;
    }
  }
  fenv_t inherited = {UINT32_C(0x03800000), UINT32_C(0x0800009f)};
  ArmEnvWrite(&inherited);
  struct ArmEnvWorker workers[4];
  pthread_t threads[4];
  int created[4];
  for (unsigned i = 0; i < 4; ++i) {
    workers[i].inherited = inherited;
    workers[i].index = i;
    workers[i].failures = 0;
    created[i] = pthread_create(&threads[i], NULL, ArmEnvThread, &workers[i]) == 0;
    failures += ArmEnvCheck(created[i], "create environment worker");
  }
  for (unsigned i = 0; i < 4; ++i) if (created[i]) {
    void* result = NULL;
    failures += ArmEnvCheck(pthread_join(threads[i], &result) == 0 && result == &workers[i],
                             "join actual environment worker");
    failures += workers[i].failures;
  }
  fenv_t state = ArmEnvRead();
  failures += ArmEnvCheck(ArmEnvEqual(&state, &inherited), "workers preserve caller environment");
  ArmEnvCallbackExpected = inherited;
  ArmEnvCallbackFailures = 0;
  ArmEnvCallbackCalls = 0;
  int values[2] = {2, 1};
  qsort(values, 2, sizeof(int), ArmEnvOuterCompare);
  state = ArmEnvRead();
  failures += ArmEnvCallbackFailures + ArmEnvCheck(ArmEnvCallbackCalls >= 2 && values[0] == 1 &&
    ArmEnvEqual(&state, &ArmEnvCallbackExpected), "nested guest-host-guest environments persist");
  ArmEnvWrite(&saved);
  if (failures == 0 && rows == 256) {
    puts("PASS arm64 environment guest: 256 pinned rows, eight-byte canaries, imported/dynamic default, errno, threads and nested callbacks");
  } else if (rows != 256) {
    failures += ArmEnvCheck(0, "all 256 environment rows executed");
  }
  return failures;
}
#endif
