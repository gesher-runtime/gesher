// Thread-exit work of the bionic shim (compat/bionic/pthread_bionic.cc) on
// every kind of thread loaded code runs on: one the shim started, one ART or a
// host library started with winpthreads, and a plain Windows thread (the
// callback threads of host code). Each sets the keys an app and ART set when
// a library attaches a thread to the VM:
//  - ART's key (winpthreads, as ART creates it): its destructor's first call
//    re-sets the key and expects the other keys' destructors to detach the
//    thread before it is called again; a second call is ART's abort ("Native
//    thread exited without calling DetachCurrentThread", Waze);
//  - the library's key (bionic), whose destructor detaches: clears ART's key;
//  - a bionic key whose destructor sets it again once: called a second time,
//    in a later round, after the detaching destructor ran;
//  - a C++ thread_local destructor (__cxa_thread_atexit_impl).
// Prints "bionic_key_exit_test all passed".

#include <pthread.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))  // BIONIC_SYSV (compat/bionic/abi_types.h)
#else
#define SYSV
#endif

extern "C" {
int SYSV gesher_bionic_pthread_key_create(int32_t* key, void (SYSV* dtor)(void*));
int SYSV gesher_bionic_pthread_setspecific(int32_t key, const void* value);
int SYSV gesher_bionic_pthread_create(uint64_t* thread, const void* attr,
                                      void* (SYSV* start)(void*), void* arg);
int SYSV gesher_bionic_pthread_join(uint64_t thread, void** retval);
int SYSV gesher_bionic_cxa_thread_atexit_impl(void (SYSV* dtor)(void*), void* obj, void* dso);
}

namespace {

pthread_key_t art_key;
int32_t detach_key;
int32_t again_key;
int failures;

struct State {
  int art_calls = 0;
  bool art_second = false;   // ART's key came round again: ART aborts here
  bool detached = false;
  int art_calls_at_detach = -1;
  int again_calls = 0;
  bool detached_before_again = false;
  bool thread_local_ran = false;
  bool thread_local_before_keys = false;
};

// ART's Thread::ThreadExitCallback.
void ArtExit(void* value) {
  auto* state = static_cast<State*>(value);
  if (state->art_calls++ == 0) {
    pthread_setspecific(art_key, value);
  } else {
    state->art_second = true;
  }
}

// The library's destructor: JavaVM::DetachCurrentThread clears ART's key.
void SYSV Detach(void* value) {
  auto* state = static_cast<State*>(value);
  state->detached = true;
  state->art_calls_at_detach = state->art_calls;
  pthread_setspecific(art_key, nullptr);
}

void SYSV Again(void* value) {
  auto* state = static_cast<State*>(value);
  if (state->again_calls++ == 0) {
    gesher_bionic_pthread_setspecific(again_key, value);
  } else {
    state->detached_before_again = state->detached;
  }
}

void SYSV ThreadLocal(void* value) {
  auto* state = static_cast<State*>(value);
  state->thread_local_ran = true;
  state->thread_local_before_keys = !state->detached && state->again_calls == 0;
}

void Attach(State* state) {
  pthread_setspecific(art_key, state);
  gesher_bionic_pthread_setspecific(detach_key, state);
  gesher_bionic_pthread_setspecific(again_key, state);
  gesher_bionic_cxa_thread_atexit_impl(ThreadLocal, state, nullptr);
}

void* SYSV BionicThread(void* arg) {
  Attach(static_cast<State*>(arg));
  return nullptr;
}

void* HostThread(void* arg) {
  Attach(static_cast<State*>(arg));
  return nullptr;
}

DWORD WINAPI WindowsThread(void* arg) {
  Attach(static_cast<State*>(arg));
  return 0;
}

void Report(const char* kind, const State& s) {
  bool ok = s.detached && !s.art_second && s.again_calls == 2 && s.detached_before_again &&
            s.thread_local_ran && s.thread_local_before_keys;
  std::printf("%s  bionic keys: %s thread detaches before ART's key comes round again\n",
              ok ? "ok  " : "FAIL", kind);
  if (!ok) {
    std::printf("      detached=%d (ART's destructor had run %d times) art_second=%d "
                "again_calls=%d detached_before_again=%d thread_local=%d before_keys=%d\n",
                s.detached, s.art_calls_at_detach, s.art_second, s.again_calls,
                s.detached_before_again, s.thread_local_ran, s.thread_local_before_keys);
    ++failures;
  }
  std::fflush(stdout);
}

}  // namespace

int main() {
  if (pthread_key_create(&art_key, ArtExit) != 0 ||
      gesher_bionic_pthread_key_create(&detach_key, Detach) != 0 ||
      gesher_bionic_pthread_key_create(&again_key, Again) != 0) {
    std::puts("FAIL  bionic keys: key creation");
    return 1;
  }

  State bionic;
  uint64_t bt = 0;
  if (gesher_bionic_pthread_create(&bt, nullptr, BionicThread, &bionic) != 0 ||
      gesher_bionic_pthread_join(bt, nullptr) != 0) {
    std::puts("FAIL  bionic keys: bionic pthread_create");
    return 1;
  }
  Report("a shim-started", bionic);

  State host;
  pthread_t ht;
  if (pthread_create(&ht, nullptr, HostThread, &host) != 0 || pthread_join(ht, nullptr) != 0) {
    std::puts("FAIL  bionic keys: winpthreads pthread_create");
    return 1;
  }
  Report("a winpthreads", host);

  State windows;
  HANDLE wt = CreateThread(nullptr, 0, WindowsThread, &windows, 0, nullptr);
  if (wt == nullptr || WaitForSingleObject(wt, 30000) != WAIT_OBJECT_0) {
    std::puts("FAIL  bionic keys: CreateThread");
    return 1;
  }
  CloseHandle(wt);
  Report("a Windows", windows);

  if (failures != 0) {
    std::printf("bionic_key_exit_test: %d failed\n", failures);
    return 1;
  }
  std::puts("bionic_key_exit_test all passed");
  return 0;
}
