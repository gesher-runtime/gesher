// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Application-owned, same-thread C callbacks. No Java, foreign-thread jump,
// returned target frame, variably modified type, or C++ destructor is involved.
// The partially sorted array is never inspected after an escaping comparator.
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kBsearch = 1, kQsort, kNestedOuter, kNearestInner };
typedef struct {
  sigjmp_buf outer;
  sigjmp_buf inner;
  pthread_t owner;
  int kind;
  int outer_calls;
  int inner_calls;
  int outer_landings;
  int inner_landings;
  int continued_host;
  int continued_nested;
  int failures;
} JumpState;

// Callback-modified observations have thread storage duration. Reading them
// after longjmp does not read indeterminate modified automatic locals.
static _Thread_local JumpState state;

static int Mask(int first, int second) {
  sigset_t mask;
  if (sigemptyset(&mask) != 0 ||
      (first && sigaddset(&mask, SIGUSR1) != 0) ||
      (second && sigaddset(&mask, SIGUSR2) != 0)) return -1;
  return sigprocmask(SIG_SETMASK, &mask, NULL);
}

static int MaskIs(int first, int second) {
  sigset_t mask;
  return sigprocmask(SIG_SETMASK, NULL, &mask) == 0 &&
         sigismember(&mask, SIGUSR1) == first &&
         sigismember(&mask, SIGUSR2) == second;
}

static void Check(const char* label, int ok) {
  if (!ok) {
    printf("FAIL  callback longjmp: %s (case %d)\n", label, state.kind);
    ++state.failures;
  }
}

static int Compare(const void* left, const void* right) {
  int a = *(const int*)left, b = *(const int*)right;
  return (a > b) - (a < b);
}

static int EscapeOuter(const void* left, const void* right) {
  (void)left;
  (void)right;
  ++state.outer_calls;
  Check("outer comparator stays on its owning thread", pthread_equal(state.owner, pthread_self()));
  Check("change mask inside comparator", Mask(0, 1) == 0);
  siglongjmp(state.outer, 23);
}

static int EscapeInner(const void* left, const void* right) {
  (void)left;
  (void)right;
  ++state.inner_calls;
  Check("nested comparator stays on its owning thread", pthread_equal(state.owner, pthread_self()));
  Check("change mask inside nested comparator", Mask(0, 1) == 0);
  if (state.kind == kNearestInner) siglongjmp(state.inner, -17);
  siglongjmp(state.outer, 23);
}

static int NestedCompare(const void* left, const void* right) {
  static const int sorted[] = {3, 5, 7, 11};
  const int key = 5;
  ++state.outer_calls;
  Check("outer nested comparator stays on its owning thread", pthread_equal(state.owner, pthread_self()));
  if (state.kind == kNearestInner) {
    // This target belongs to a still-live comparator, rather than the oldest
    // active translated entry. A successful escape must resume this frame.
    switch (sigsetjmp(state.inner, 1)) {
      case 0:
        (void)bsearch(&key, sorted, 4, sizeof sorted[0], EscapeInner);
        ++state.continued_nested;
        break;
      case -17:
        ++state.inner_landings;
        break;
      default:
        Check("inner jump return value", 0);
    }
    Check("inner jump restores its saved mask", MaskIs(1, 0));
    return Compare(left, right);
  }
  (void)bsearch(&key, sorted, 4, sizeof sorted[0], EscapeInner);
  ++state.continued_nested;
  return Compare(left, right);
}

static int OneCase(int kind) {
  sigset_t original;
  int values[] = {11, 3, 7, 5};
  static const int sorted[] = {3, 5, 7, 11};
  const int key = 7;
  memset(&state, 0, sizeof state);
  state.kind = kind;
  state.owner = pthread_self();
  if (sigprocmask(SIG_SETMASK, NULL, &original) != 0) return 1;
  if (Mask(1, 0) != 0) {
    (void)sigprocmask(SIG_SETMASK, &original, NULL);
    return 1;
  }
  // Use setjmp in a standard-permitted controlling expression. State changed
  // by a callback resides in TLS, and the original mask is never changed.
  switch (sigsetjmp(state.outer, 1)) {
    case 0:
      if (kind == kBsearch) (void)bsearch(&key, sorted, 4, sizeof sorted[0], EscapeOuter);
      else qsort(values, 4, sizeof values[0], kind == kQsort ? EscapeOuter : NestedCompare);
      ++state.continued_host;
      break;
    case 23:
      ++state.outer_landings;
      break;
    default:
      Check("outer jump return value", 0);
  }
  if (kind == kNearestInner) {
    Check("inner target resumes without escaping its enclosing sort",
          state.continued_host == 1 && state.outer_landings == 0 &&
          state.outer_calls > 0 && state.inner_landings == state.outer_calls &&
          state.inner_calls == state.outer_calls && state.continued_nested == 0);
    Check("enclosing sort completes normally", memcmp(values, sorted, sizeof values) == 0);
  } else {
    Check("escaped native call never continues", state.continued_host == 0);
    Check("outer target resumes exactly once", state.outer_landings == 1 && state.outer_calls == 1);
    Check("escaped nested search never continues", state.continued_nested == 0);
    Check("expected nested callback count", state.inner_calls == (kind == kNestedOuter ? 1 : 0));
  }
  Check("outer environment retains its saved signal mask", MaskIs(1, 0));

  // Fresh automatic objects are initialized after the jump. An interrupted
  // qsort's partially modified array is intentionally not read in this path.
  int fresh[] = {11, 3, 7, 5};
  qsort(fresh, 4, sizeof fresh[0], Compare);
  Check("ordinary callback sort works after the jump", memcmp(fresh, sorted, sizeof fresh) == 0);
  Check("ordinary callback search works after the jump", bsearch(&key, fresh, 4, sizeof fresh[0], Compare) == &fresh[2]);
  Check("restore original caller mask", sigprocmask(SIG_SETMASK, &original, NULL) == 0);
  return state.failures;
}

static void* ThreadCases(void* unused) {
  (void)unused;
  int failures = 0;
  for (int kind = kBsearch; kind <= kNearestInner; ++kind) failures += OneCase(kind);
  return (void*)(intptr_t)failures;
}

static int CallbackLongjmpContracts(int selected) {
  if (selected < 0 || selected > kNearestInner) return 2;
  int failures = 0;
  printf("callback longjmp contracts: begin (case %d)\n", selected);
  if (selected != 0) {
    failures = OneCase(selected);
  } else {
    // Repeated escapes must not leave stale entry ownership or nesting state.
    for (int repeat = 0; repeat < 16; ++repeat)
      for (int kind = kBsearch; kind <= kNearestInner; ++kind) failures += OneCase(kind);
    pthread_t threads[4];
    int started = 0;
    for (; started < 4; ++started)
      if (pthread_create(&threads[started], NULL, ThreadCases, NULL) != 0) break;
    if (started != 4) ++failures;
    for (int i = 0; i < started; ++i) {
      void* result = NULL;
      if (pthread_join(threads[i], &result) != 0 || result != NULL) ++failures;
    }
  }
  printf("callback longjmp contracts: %s (%d failures; %d cases)\n",
         failures == 0 ? "all passed" : "FAILED", failures, selected ? 1 : 80);
  return failures == 0 ? 0 : 1;
}

int gesher_selftest(const char* unused) {
  (void)unused;
  const char* selected = getenv("GESHER_CALLBACK_JUMP_CASE");
  return CallbackLongjmpContracts(selected != NULL ? atoi(selected) : 0);
}

#if defined(GESHER_CALLBACK_LONGJMP_MAIN)
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  return CallbackLongjmpContracts(argc == 2 ? atoi(argv[1]) : 0);
}
#endif
