// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Android's libstdc++.so exports through the real Bionic shim (issue #480: a
// library linked against libstdc++.so got an unresolved operator new stub
// that returned null, and crashed on its first allocation). Checks
// operator new/delete and their nothrow forms against the shim's malloc and
// free, dlopen("libstdc++.so") + dlsym, and the static-initialisation guards
// (one initialiser among racing threads, abort lets the next one run).
// Prints "bionic_libstdcxx_test all passed".
#include <gesher/bionic_shim.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))  // BIONIC_SYSV (compat/bionic/abi_types.h)
#else
#define SYSV
#endif

namespace {
using New = void*(SYSV*)(size_t);
using NewNothrow = void*(SYSV*)(size_t, const void*);
using Delete = void(SYSV*)(void*);
using DeleteNothrow = void(SYSV*)(void*, const void*);
using Free = void(SYSV*)(void*);
using GuardAcquire = int(SYSV*)(uint64_t*);
using GuardRelease = void(SYSV*)(uint64_t*);
using DlOpen = void*(SYSV*)(const char*, int);
using DlSym = void*(SYSV*)(void*, const char*);

int assertions, failures;

bool Check(const char* name, bool success) {
  ++assertions;
  if (!success) ++failures;
  std::printf("bionic_libstdcxx: %s %s\n", name, success ? "PASS" : "FAIL");
  return success;
}

template <class T>
T Lookup(const char* name) {
  return reinterpret_cast<T>(gesher::bionic::LookupBionicSymbol(name));
}

void Allocation() {
  auto new_one = Lookup<New>("_Znwm");
  auto new_array = Lookup<New>("_Znam");
  auto new_nothrow = Lookup<NewNothrow>("_ZnwmRKSt9nothrow_t");
  auto new_array_nothrow = Lookup<NewNothrow>("_ZnamRKSt9nothrow_t");
  auto del = Lookup<Delete>("_ZdlPv");
  auto del_array = Lookup<Delete>("_ZdaPv");
  auto del_nothrow = Lookup<DeleteNothrow>("_ZdlPvRKSt9nothrow_t");
  auto del_array_nothrow = Lookup<DeleteNothrow>("_ZdaPvRKSt9nothrow_t");
  auto free = Lookup<Free>("free");
  if (!Check("resolve operator new, delete and their nothrow and array forms",
             new_one && new_array && new_nothrow && new_array_nothrow && del && del_array &&
                 del_nothrow && del_array_nothrow && free)) {
    return;
  }
  const char nothrow_tag = 0;
  struct Case {
    const char* name;
    void* p;
    size_t size;
  } cases[] = {
      {"operator new(64)", new_one(64), 64},
      {"operator new[](4096)", new_array(4096), 4096},
      {"operator new(0)", new_one(0), 0},
      {"operator new(64, nothrow)", new_nothrow(64, &nothrow_tag), 64},
      {"operator new[](64, nothrow)", new_array_nothrow(64, &nothrow_tag), 64},
  };
  char name[96];
  for (Case& c : cases) {
    std::snprintf(name, sizeof(name), "%s returns usable 16-byte-aligned storage", c.name);
    bool ok = c.p != nullptr && reinterpret_cast<uintptr_t>(c.p) % 16 == 0;
    if (ok && c.size) {
      std::memset(c.p, 0x5a, c.size);
      ok = static_cast<unsigned char*>(c.p)[c.size - 1] == 0x5a;
    }
    Check(name, ok);
  }
  del(cases[0].p);
  del_array(cases[1].p);
  del(cases[2].p);
  del_nothrow(cases[3].p, &nothrow_tag);
  del_array_nothrow(cases[4].p, &nothrow_tag);
  Check("operator delete and delete[] release the storage", true);

  // Storage from operator new is malloc's: free accepts it, as bionic's.
  void* shared = new_one(32);
  free(shared);
  Check("free releases operator new's storage", shared != nullptr);
  del(nullptr);
  del_array(nullptr);
  Check("operator delete(nullptr) is a no-op", true);
}

void PlatformLibrary() {
  auto dlopen = Lookup<DlOpen>("dlopen");
  auto dlsym = Lookup<DlSym>("dlsym");
  if (!Check("resolve dlopen and dlsym", dlopen && dlsym)) return;
  void* handle = dlopen("libstdc++.so", 0);
  Check("dlopen(\"libstdc++.so\") is a platform handle", handle != nullptr);
  void* system = dlopen("/system/lib64/libstdc++.so", 0);
  Check("dlopen(\"/system/lib64/libstdc++.so\") is the same handle", system == handle);
  if (handle == nullptr) return;
  Check("dlsym(libstdc++.so, \"_Znwm\") is the shim's operator new",
        dlsym(handle, "_Znwm") == gesher::bionic::LookupBionicSymbol("_Znwm"));
  Check("dlsym(libstdc++.so, \"__cxa_guard_acquire\") is the shim's guard",
        dlsym(handle, "__cxa_guard_acquire") ==
            gesher::bionic::LookupBionicSymbol("__cxa_guard_acquire"));
}

bool Complete(const uint64_t* guard) {
  return __atomic_load_n(reinterpret_cast<const uint8_t*>(guard), __ATOMIC_ACQUIRE) != 0;
}

void Guards() {
  auto acquire = Lookup<GuardAcquire>("__cxa_guard_acquire");
  auto release = Lookup<GuardRelease>("__cxa_guard_release");
  auto abort = Lookup<GuardRelease>("__cxa_guard_abort");
  bool pure = gesher::bionic::LookupBionicSymbol("__cxa_pure_virtual") != nullptr &&
              gesher::bionic::LookupBionicSymbol("_ZSt7nothrow") != nullptr;
  if (!Check("resolve __cxa_guard_acquire, release, abort, __cxa_pure_virtual and std::nothrow",
             acquire && release && abort && pure)) {
    return;
  }

  alignas(8) uint64_t guard = 0;
  Check("acquire of a fresh guard runs the initialiser", acquire(&guard) == 1);
  Check("the guard is not complete while initialising", !Complete(&guard));
  release(&guard);
  Check("release marks the guard's first byte complete", Complete(&guard));
  Check("acquire after release skips the initialiser", acquire(&guard) == 0);

  alignas(8) uint64_t retried = 0;
  Check("acquire before an aborted initialiser", acquire(&retried) == 1);
  abort(&retried);
  Check("abort leaves the guard incomplete", !Complete(&retried));
  Check("acquire after abort runs the initialiser again", acquire(&retried) == 1);
  release(&retried);

  // Racing threads: exactly one initialises; the others wait for its release
  // and then see the guard complete.
  constexpr int kThreads = 8;
  for (int round = 0; round < 50; ++round) {
    alignas(8) uint64_t raced = 0;
    std::atomic<int> initialisers{0}, ready{0}, saw_incomplete_after{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&] {
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        if (acquire(&raced) == 1) {
          initialisers.fetch_add(1);
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          release(&raced);
        } else if (!Complete(&raced)) {
          saw_incomplete_after.fetch_add(1);
        }
      });
    }
    while (ready.load() != kThreads) std::this_thread::yield();
    go.store(true);
    for (std::thread& t : threads) t.join();
    if (initialisers.load() != 1 || saw_incomplete_after.load() != 0) {
      std::printf("bionic_libstdcxx: round %d: %d initialisers, %d early returns\n", round,
                  initialisers.load(), saw_incomplete_after.load());
      Check("racing threads: one initialiser, the others wait for it", false);
      return;
    }
  }
  Check("racing threads: one initialiser, the others wait for it (50 rounds)", true);
}
}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  gesher::bionic::InstallBionicShim();
  Allocation();
  PlatformLibrary();
  Guards();
  std::printf("bionic_libstdcxx: %d assertions, %d failures\n", assertions, failures);
  if (!failures) std::puts("bionic_libstdcxx_test all passed");
  return failures ? 1 : 0;
}
