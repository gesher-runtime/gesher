// bionic's libstdc++.so (bionic/libc/libstdc++.map.txt) from an app library:
// built with the NDK's "system" C++ runtime (-nostdlib++ -lstdc++), so the
// library's own operator new/delete, static-local guards and pure-virtual
// vtable slot bind to libstdc++.so through its DT_NEEDED entry, and it also
// dlopen()s "libstdc++.so" and dlsym()s every export, as packers do
// (NetEase Yidun's libnesec.so). Built for x86_64 (elf_load_test.exe) and
// arm64-v8a (arm64_elf_test.exe); gesher_selftest() returns the failure count.
//
// The fatal paths (operator new failing, __cxa_pure_virtual), which end the
// process, are tests/native2/libstdcxx_fatal_test.cpp's (native2 track).

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <new>

extern "C" int __cxa_guard_acquire(uint64_t*);
extern "C" void __cxa_guard_release(uint64_t*);
extern "C" void __cxa_guard_abort(uint64_t*);
extern "C" void __cxa_pure_virtual();

namespace {

int Load(const int* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void Store(int* p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
int FetchAdd(int* p, int v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
int FetchSub(int* p, int v) { return __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST); }

int checks, failures;
void Check(const char* label, bool ok) {
  ++checks;
  if (!ok) {
    ++failures;
    printf("FAIL libstdcxx: %s\n", label);
  } else {
    printf("ok   libstdcxx: %s\n", label);
  }
  fflush(stdout);
}

void SleepMs(long ms) {
  struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&t, nullptr);
}

int64_t NowMs() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t{t.tv_sec} * 1000 + t.tv_nsec / 1000000;
}

using GuardFn = int (*)(uint64_t*);
using GuardEndFn = void (*)(uint64_t*);
GuardFn g_acquire;
GuardEndFn g_release, g_abort;

// --- exports --------------------------------------------------------------------------

const char* const kExports[] = {
    "_ZSt7nothrow",        "_Znwm",
    "_Znam",               "_ZnwmRKSt9nothrow_t",
    "_ZnamRKSt9nothrow_t", "_ZdlPv",
    "_ZdaPv",              "_ZdlPvRKSt9nothrow_t",
    "_ZdaPvRKSt9nothrow_t", "__cxa_guard_acquire",
    "__cxa_guard_release", "__cxa_guard_abort",
    "__cxa_pure_virtual",
};

void CheckExports() {
  void* h = dlopen("libstdc++.so", RTLD_NOW);
  Check("dlopen(\"libstdc++.so\") succeeds", h != nullptr);
  if (h == nullptr) {
    printf("      dlerror: %s\n", dlerror());
    return;
  }
  void* full = dlopen("/system/lib64/libstdc++.so", RTLD_NOW);
  Check("dlopen(\"/system/lib64/libstdc++.so\") is the same library", full == h);
  for (const char* name : kExports) {
    char label[96];
    snprintf(label, sizeof(label), "dlsym(libstdc++.so, \"%s\")", name);
    Check(label, dlsym(h, name) != nullptr);
  }
  // new.cpp defines the sized deletes, but libstdc++.map.txt keeps them local.
  Check("the sized operator delete is not exported", dlsym(h, "_ZdlPvm") == nullptr);
  dlerror();

  // The library's own DT_NEEDED bindings are the same functions. A translated
  // arm64 guest binds every host import to a guest trampoline, while dlsym
  // returns the host entry point the translator calls directly (docs/arm64.md),
  // so only data addresses compare equal there; the calls below go through both.
#if defined(__x86_64__)
  Check("bound __cxa_guard_acquire is libstdc++.so's",
        dlsym(h, "__cxa_guard_acquire") == reinterpret_cast<void*>(&__cxa_guard_acquire));
  Check("bound __cxa_pure_virtual is libstdc++.so's",
        dlsym(h, "__cxa_pure_virtual") == reinterpret_cast<void*>(&__cxa_pure_virtual));
  void* (*bound_new)(size_t) = &::operator new;
  Check("bound operator new is libstdc++.so's", dlsym(h, "_Znwm") == reinterpret_cast<void*>(bound_new));
#endif
  Check("std::nothrow is libstdc++.so's", dlsym(h, "_ZSt7nothrow") == static_cast<const void*>(&std::nothrow));

  g_acquire = reinterpret_cast<GuardFn>(dlsym(h, "__cxa_guard_acquire"));
  g_release = reinterpret_cast<GuardEndFn>(dlsym(h, "__cxa_guard_release"));
  g_abort = reinterpret_cast<GuardEndFn>(dlsym(h, "__cxa_guard_abort"));
}

// --- new.cpp --------------------------------------------------------------------------

void CheckAllocation() {
  using NewFn = void* (*)(size_t);
  using NothrowNewFn = void* (*)(size_t, const std::nothrow_t&);
  using DeleteFn = void (*)(void*);
  void* h = dlopen("libstdc++.so", RTLD_NOW);
  if (h == nullptr) return;
  auto new_fn = reinterpret_cast<NewFn>(dlsym(h, "_Znwm"));
  auto new_array = reinterpret_cast<NewFn>(dlsym(h, "_Znam"));
  auto new_nothrow = reinterpret_cast<NothrowNewFn>(dlsym(h, "_ZnwmRKSt9nothrow_t"));
  auto new_array_nothrow = reinterpret_cast<NothrowNewFn>(dlsym(h, "_ZnamRKSt9nothrow_t"));
  auto delete_fn = reinterpret_cast<DeleteFn>(dlsym(h, "_ZdlPv"));
  auto delete_array = reinterpret_cast<DeleteFn>(dlsym(h, "_ZdaPv"));
  if (!new_fn || !new_array || !new_nothrow || !new_array_nothrow || !delete_fn || !delete_array)
    return;

  // operator new is malloc: its memory goes back through free(), and malloc's
  // through operator delete.
  auto* p = static_cast<unsigned char*>(new_fn(64));
  Check("operator new(64) returns memory", p != nullptr);
  if (p != nullptr) {
    memset(p, 0xa5, 64);
    free(p);
  }
  void* m = malloc(48);
  Check("malloc'd memory released with operator delete", m != nullptr);
  delete_fn(m);
  auto* a = static_cast<unsigned char*>(new_array(256));
  Check("operator new[](256) returns memory", a != nullptr);
  if (a != nullptr) memset(a, 0x5a, 256);
  delete_array(a);
  void* z1 = new_fn(0);
  void* z2 = new_fn(0);
  Check("operator new(0) returns distinct non-null pointers", z1 != nullptr && z2 != nullptr && z1 != z2);
  delete_fn(z1);
  delete_fn(z2);
  delete_fn(nullptr);
  delete_array(nullptr);

  void* n = new_nothrow(32, std::nothrow);
  Check("nothrow operator new(32) returns memory", n != nullptr);
  free(n);
  Check("nothrow operator new(SIZE_MAX) returns null", new_nothrow(SIZE_MAX, std::nothrow) == nullptr);
  Check("nothrow operator new[](SIZE_MAX) returns null",
        new_array_nothrow(SIZE_MAX, std::nothrow) == nullptr);
  Check("nothrow operator new(PTRDIFF_MAX) returns null",
        new_nothrow(static_cast<size_t>(PTRDIFF_MAX), std::nothrow) == nullptr);

  // Through the compiler: new/delete expressions bind to the same library.
  int* v = new int[16];
  for (int i = 0; i < 16; ++i) v[i] = i;
  Check("new int[16] / delete[]", v[15] == 15);
  delete[] v;
  int* q = new (std::nothrow) int(7);
  Check("new (std::nothrow) int", q != nullptr && *q == 7);
  delete q;
}

// --- __cxa_guard.cpp: the state word --------------------------------------------------

int32_t Word(const uint64_t* guard) {
  return __atomic_load_n(reinterpret_cast<const int32_t*>(guard), __ATOMIC_ACQUIRE);
}
uint8_t FirstByte(const uint64_t* guard) {
  return __atomic_load_n(reinterpret_cast<const uint8_t*>(guard), __ATOMIC_ACQUIRE);
}

void CheckGuardProtocol() {
  if (!g_acquire || !g_release || !g_abort) return;
  uint64_t guard = 0;
  Check("acquire of a fresh guard returns 1", g_acquire(&guard) == 1);
  Check("acquire leaves the guard's first byte zero (construction underway)", FirstByte(&guard) == 0);
  Check("acquire marks construction underway (0x100)", Word(&guard) == 0x100);
  g_release(&guard);
  Check("release sets the guard to 1", guard == 1);
  Check("release makes the first byte non-zero (bit 0 set)", (FirstByte(&guard) & 1) == 1);
  Check("acquire of a completed guard returns 0", g_acquire(&guard) == 0);
  Check("a completed guard stays 1", guard == 1);

  uint64_t aborted = 0;
  Check("acquire before abort returns 1", g_acquire(&aborted) == 1);
  g_abort(&aborted);
  Check("abort returns the guard to 0", aborted == 0);
  Check("acquire after abort returns 1 again", g_acquire(&aborted) == 1);
  g_release(&aborted);
  Check("release after a retry completes the guard", g_acquire(&aborted) == 0);

  // Only the first four bytes are the state; the upper half is untouched.
  uint64_t high = uint64_t{0xabcdef01} << 32;
  Check("acquire with the guard's upper half set returns 1", g_acquire(&high) == 1);
  g_release(&high);
  Check("the guard's upper half is left alone", (high >> 32) == 0xabcdef01 && (high & 0xffffffff) == 1);
}

// --- __cxa_guard.cpp: threads racing on one guard -------------------------------------

constexpr int kThreads = 8;
constexpr int kRounds = 40;

struct Race {
  pthread_barrier_t start, end;
  uint64_t guard;
  int winners;        // acquires that returned 1 this round
  int aborts_left;    // abort rounds: the first winner aborts once
  int value;          // what the constructor published
  int stale;          // a 0 from acquire before the value was published
  int waiter_seen;    // rounds in which a waiter announced itself (0x200)
  int waiter_timeout; // rounds in which none did within 10 s
  int round;
};
Race g_race;

// The constructing thread waits until another thread has announced itself as
// a waiter (0x200), so every round exercises the sleeping path.
bool AwaitWaiter(const uint64_t* guard) {
  int64_t deadline = NowMs() + 10000;
  while (Word(guard) != 0x200) {
    if (NowMs() > deadline) return false;
    sched_yield();
  }
  return true;
}

void* RaceThread(void*) {
  for (int r = 0; r < kRounds; ++r) {
    pthread_barrier_wait(&g_race.start);
    for (;;) {
      int got = g_acquire(&g_race.guard);
      if (got == 0) {
        if (Load(&g_race.value) != g_race.round + 1) FetchAdd(&g_race.stale, 1);
        break;
      }
      FetchAdd(&g_race.winners, 1);
      bool announced = AwaitWaiter(&g_race.guard);
      FetchAdd(announced ? &g_race.waiter_seen : &g_race.waiter_timeout, 1);
      SleepMs(1);
      if (FetchSub(&g_race.aborts_left, 1) > 0) {
        // The construction failed (an exception, in a library that has them):
        // a waiter takes over, and this thread tries again.
        g_abort(&g_race.guard);
        continue;
      }
      Store(&g_race.value, g_race.round + 1);
      g_release(&g_race.guard);
      break;
    }
    pthread_barrier_wait(&g_race.end);
  }
  return nullptr;
}

void CheckGuardRace() {
  if (!g_acquire || !g_release || !g_abort) return;
  pthread_barrier_init(&g_race.start, nullptr, kThreads + 1);
  pthread_barrier_init(&g_race.end, nullptr, kThreads + 1);
  pthread_t threads[kThreads];
  int created = 0;
  for (int i = 0; i < kThreads; ++i) created += pthread_create(&threads[i], nullptr, RaceThread, nullptr) == 0;
  Check("race threads started", created == kThreads);
  if (created != kThreads) return;  // the barriers cannot complete
  int bad_winners = 0, bad_guard = 0;
  for (int r = 0; r < kRounds; ++r) {
    bool abort_round = (r % 2) == 1;
    g_race.guard = 0;
    g_race.round = r;
    Store(&g_race.winners, 0);
    Store(&g_race.aborts_left, abort_round ? 1 : 0);
    pthread_barrier_wait(&g_race.start);
    pthread_barrier_wait(&g_race.end);
    if (Load(&g_race.winners) != (abort_round ? 2 : 1)) ++bad_winners;
    if (g_race.guard != 1 || Load(&g_race.value) != r + 1) ++bad_guard;
  }
  for (int i = 0; i < kThreads; ++i) pthread_join(threads[i], nullptr);
  pthread_barrier_destroy(&g_race.start);
  pthread_barrier_destroy(&g_race.end);
  char detail[160];
  snprintf(detail, sizeof(detail),
           "%d threads x %d rounds: one constructor per round, a second after each abort", kThreads,
           kRounds);
  Check(detail, bad_winners == 0);
  Check("every round ends with a completed guard and the published value", bad_guard == 0);
  Check("no thread saw a completed guard before the value was published", Load(&g_race.stale) == 0);
  Check("waiters announced themselves (0x200) and slept until woken",
        Load(&g_race.waiter_seen) == kRounds * 3 / 2 && Load(&g_race.waiter_timeout) == 0);
}

// --- compiler-generated guards and the pure-virtual vtable slot -----------------------

int g_constructions;

struct Slow {
  int id;
  explicit Slow(int i) : id(i) {
    FetchAdd(&g_constructions, 1);
    SleepMs(50);  // the other threads reach the guard meanwhile
  }
};

__attribute__((noinline)) Slow* Instance(int seed) {
  static Slow instance(seed + 41);
  return &instance;
}

struct InstanceArgs {
  pthread_barrier_t* start;
  Slow* result;
};

void* InstanceThread(void* opaque) {
  auto* args = static_cast<InstanceArgs*>(opaque);
  pthread_barrier_wait(args->start);
  args->result = Instance(1);
  return nullptr;
}

struct Shape {
  virtual ~Shape() {}
  virtual int Sides() const = 0;
};
struct Square : Shape {
  int Sides() const override { return 4; }
};

void CheckCompilerGenerated() {
  pthread_barrier_t start;
  pthread_barrier_init(&start, nullptr, kThreads);
  pthread_t threads[kThreads];
  InstanceArgs args[kThreads];
  int created = 0;
  for (int i = 0; i < kThreads; ++i) {
    args[i] = {&start, nullptr};
    created += pthread_create(&threads[i], nullptr, InstanceThread, &args[i]) == 0;
  }
  Check("static-local threads started", created == kThreads);
  if (created != kThreads) return;
  for (int i = 0; i < kThreads; ++i) pthread_join(threads[i], nullptr);
  pthread_barrier_destroy(&start);
  bool same = true;
  for (int i = 0; i < kThreads; ++i) same &= args[i].result == args[0].result && args[i].result != nullptr;
  Check("a function-local static is constructed once by racing threads",
        Load(&g_constructions) == 1);
  Check("every thread sees the one constructed object", same && args[0].result->id == 42);

  Shape* s = new Square;
  Check("a virtual call through a class with a pure virtual base", s->Sides() == 4);
  delete s;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int gesher_selftest() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  CheckExports();
  CheckAllocation();
  CheckGuardProtocol();
  CheckGuardRace();
  CheckCompilerGenerated();
  if (failures == 0) printf("libstdcxx: all passed (%d checks)\n", checks);
  else printf("libstdcxx: %d of %d checks failed\n", failures, checks);
  return failures;
}
