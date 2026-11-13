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
#include <gesher/bionic_shim.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

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
int SYSV gesher_bionic_pthread_detach(uint64_t thread);
uint64_t SYSV gesher_bionic_pthread_self();
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

// Exercise the actual exported bionic adapters, with a live target at every
// cross-thread call. Native completion is observed before freeing its state.
using SetName = int (SYSV*)(uint64_t, const char*);
using GetName = int (SYSV*)(uint64_t, char*, size_t);
using Prctl = int (SYSV*)(int, uint64_t, uint64_t, uint64_t, uint64_t);
SetName set_name;
GetName get_name;
Prctl prctl_name;
const char* kUnicodeName = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
void NameCheck(bool result, const char* label) {
  if (!result) { ++failures; std::printf("FAIL bionic names: %s\n", label); }
}
void NameRequire(bool result, const char* label) {
  if (!result) { std::fprintf(stderr, "FAIL bionic names: %s\n", label); ExitProcess(1); }
}
void NameWait(HANDLE event) {
  NameRequire(event && WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "event/completion timeout");
}
struct NameState {
  HANDLE ready = nullptr, read = nullptr, done = nullptr, release = nullptr, native = nullptr;
  uint64_t id = 0;
  bool matched = false;
};
void NamingThread(NameState* state) {
  state->id = gesher_bionic_pthread_self();
  NameRequire(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                              &state->native, SYNCHRONIZE, FALSE, 0) != 0, "retain own thread");
  NameRequire(SetEvent(state->ready) != 0, "signal ready");
  NameWait(state->read);
  char name[16]{};
  PWSTR description = nullptr;
  state->matched = get_name(state->id, name, sizeof(name)) == 0 &&
                   std::strcmp(name, kUnicodeName) == 0 &&
                   SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) &&
                   description && std::wcscmp(description, L"\x05e9\x05dc\x05d5\x05dd") == 0;
  if (description) LocalFree(description);
  NameRequire(SUCCEEDED(SetThreadDescription(GetCurrentThread(), L"\x05e9\x05dc\x05d5\x05dd\x05e9\x05dc\x05d5\x05dd\x05e9")),
              "set long native description");
  NameRequire(SetEvent(state->done) != 0, "signal description");
  NameWait(state->release);
}
void* SYSV BionicNamingThread(void* state) { NamingThread(static_cast<NameState*>(state)); return nullptr; }
void* HostNamingThread(void* state) { NamingThread(static_cast<NameState*>(state)); return nullptr; }
DWORD WINAPI WindowsNamingThread(void* state) { NamingThread(static_cast<NameState*>(state)); return 0; }
void CheckNamingPeer(int kind) {
  NameState state;
  state.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  state.read = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  state.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  state.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  NameRequire(state.ready && state.read && state.done && state.release, "create barriers");
  uint64_t bt = 0;
  pthread_t ht{};
  HANDLE wt = nullptr;
  if (kind <= 1) NameRequire(gesher_bionic_pthread_create(&bt, nullptr, BionicNamingThread, &state) == 0, "bionic create");
  else if (kind == 2) NameRequire(pthread_create(&ht, nullptr, HostNamingThread, &state) == 0, "host create");
  else { wt = CreateThread(nullptr, 0, WindowsNamingThread, &state, 0, nullptr); NameRequire(wt != nullptr, "Windows create"); }
  NameWait(state.ready);
  if (kind == 1) NameRequire(gesher_bionic_pthread_detach(bt) == 0, "detach live target");
  char name[16]{};
  NameCheck(set_name(state.id, kUnicodeName) == 0, "cross-thread UTF-8 setter");
  NameCheck(get_name(state.id, name, sizeof(name)) == 0 && std::strcmp(name, kUnicodeName) == 0,
            "cross-thread getter");
  NameCheck(get_name(state.id, name, 15) == 34, "short buffer ERANGE");
  NameCheck(set_name(state.id, "sixteen-bytes-xx") == 34, "long name ERANGE");
  NameRequire(SetEvent(state.read) != 0, "release reader");
  NameWait(state.done);
  NameCheck(state.matched, "target self getter and Windows description agree");
  NameCheck(get_name(state.id, name, sizeof(name)) == 0 &&
            std::strcmp(name, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\xd7\xa9\xd7\x9c\xd7\x95") == 0,
            "long UTF-8 description stops at a character boundary");
  NameRequire(SetEvent(state.release) != 0, "release target");
  NameWait(state.native);
  if (kind == 0) NameRequire(gesher_bionic_pthread_join(bt, nullptr) == 0, "bionic join");
  if (kind == 2) NameRequire(pthread_join(ht, nullptr) == 0, "host join");
  if (wt) CloseHandle(wt);
  CloseHandle(state.native);
  CloseHandle(state.ready); CloseHandle(state.read); CloseHandle(state.done); CloseHandle(state.release);
}
void ThreadNames() {
  set_name = reinterpret_cast<SetName>(gesher::bionic::LookupBionicSymbol("pthread_setname_np"));
  get_name = reinterpret_cast<GetName>(gesher::bionic::LookupBionicSymbol("pthread_getname_np"));
  prctl_name = reinterpret_cast<Prctl>(gesher::bionic::LookupBionicSymbol("prctl"));
  NameRequire(set_name && get_name && prctl_name, "exported adapters");
  uint64_t self = gesher_bionic_pthread_self();
  char name[16]{};
  NameCheck(set_name(self, "bionic-name") == 0, "self set");
  NameCheck(get_name(self, name, sizeof(name)) == 0 && std::strcmp(name, "bionic-name") == 0, "self get");
  NameCheck(get_name(UINT64_MAX, name, sizeof(name)) == 3, "unknown identity ESRCH");
  NameCheck(set_name(UINT64_MAX, "unknown") == 3, "unknown setter ESRCH");
  NameCheck(prctl_name(15, reinterpret_cast<uint64_t>("12345678901234\xd7\xa9"), 0, 0, 0) == 0,
            "prctl truncates UTF-8 safely");
  NameCheck(prctl_name(16, reinterpret_cast<uint64_t>(name), 0, 0, 0) == 0 &&
            std::strcmp(name, "12345678901234") == 0, "prctl shares the name");
  NameCheck(prctl_name(15, 0, 0, 0, 0) == -1, "prctl rejects null");
  for (int kind = 0; kind != 4; ++kind) CheckNamingPeer(kind);
}

}  // namespace

int main() {
  ThreadNames();
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
