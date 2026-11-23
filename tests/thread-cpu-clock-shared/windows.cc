// Real Win32 measurements of the isolated shared winpthread supplier.
// No fake target handles and no production provider/default changes.
#include "peer.h"
#include <windows.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
[[noreturn]] void Fail(const char* message) {
  std::fprintf(stderr, "FAIL N28 shared supplier: %s\n", message);
  std::fflush(nullptr);
  // Never unwind storage referenced by a target whose exit was not confirmed.
  TerminateProcess(GetCurrentProcess(), 1);
  std::_Exit(1);
}
void Require(bool ok, const char* message) { if (!ok) Fail(message); }
void Pass(const char* message) { std::printf("PASS N28 shared supplier: %s\n", message); }
void Close(HANDLE handle) { Require(handle && CloseHandle(handle), "close owned handle"); }
uint64_t FileTicks(FILETIME time) {
  return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}
uint64_t NativeNanos(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  Require(GetThreadTimes(thread, &created, &exited, &kernel, &user), "independent GetThreadTimes");
  return (FileTicks(kernel) + FileTicks(user)) * 100;
}
int64_t Nanos(const timespec& time) { return int64_t{time.tv_sec} * 1000000000 + time.tv_nsec; }
DWORD Handles() {
  DWORD count = 0;
  Require(GetProcessHandleCount(GetCurrentProcess(), &count), "get process handle count");
  return count;
}

const clock_peer_api* peer;
using lookup_record_fn = void* (*)(pthread_t);
lookup_record_fn lookup_record;
pthread_key_t cleanup_key;
struct Actor {
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE oracle = nullptr;
  pthread_t id = 0;
  std::atomic<bool> busy{false};
  bool pthread_exit = false;
  bool test_cancel = false;
  bool check_cleanup = false;
  std::atomic<int> cleanup_calls{0};
  clock_exit_gate* exit_gate = nullptr;
};
void Cleanup(void* raw) {
  if (raw) ++static_cast<Actor*>(raw)->cleanup_calls;
}
void* Run(void* raw) {
  auto& actor = *static_cast<Actor*>(raw);
  actor.id = pthread_self();
  Require(peer->self() == actor.id, "same thread has one identity in both images");
  if (actor.check_cleanup)
    Require(pthread_setspecific(cleanup_key, &actor) == 0, "install actual thread key destructor");
  if (actor.exit_gate)
    Require(peer->arm_exit_gate(actor.exit_gate) == 0, "arm later-loaded peer final-exit callback");
  Require(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                          &actor.oracle, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                          FALSE, 0), "target publishes separately owned native oracle");
  Require(SetEvent(actor.ready), "publish target identity");
  while (WaitForSingleObject(actor.release, 0) == WAIT_TIMEOUT) {
    if (actor.busy.load()) {
      for (int i = 0; i < 10000; ++i) MemoryBarrier();
    } else {
      WaitForSingleObject(actor.release, 1);
    }
    if (actor.test_cancel) pthread_testcancel();
  }
  if (actor.pthread_exit) pthread_exit(reinterpret_cast<void*>(uintptr_t{259}));
  return reinterpret_cast<void*>(uintptr_t{7});
}
DWORD WINAPI NativeRun(void* actor) { Run(actor); return 259; }
void Ready(Actor& actor) {
  Require(actor.ready && actor.release && WaitForSingleObject(actor.ready, 15000) == WAIT_OBJECT_0,
          "real target ready before deadline");
}
void Retire(Actor& actor) {
  Require(WaitForSingleObject(actor.oracle, 15000) == WAIT_OBJECT_0, "real target actually terminated");
  Close(actor.oracle); Close(actor.ready); Close(actor.release);
}
clockid_t Acquire(Actor& actor) {
  Ready(actor);
  clockid_t first = 123, second = 456;
  errno = EDOM;
  Require(pthread_getcpuclockid(actor.id, &first) == 0 && errno == EDOM,
          "public acquisition preserves errno");
  Require(peer->clock_id(actor.id, &second) == 0 && second == first && first < 0,
          "cross-image acquisition shares stable target clock");
  Require((static_cast<unsigned>(first) & 7) == 6, "host opaque clock preserves thread/SCHED tag");
  return first;
}
void Brackets(Actor& actor, clockid_t clock) {
  for (int i = 0; i < 32; ++i) {
    timespec time{};
    uint64_t before = NativeNanos(actor.oracle);
    int result = i % 2 ? peer->read(clock, &time) : clock_gettime(clock, &time);
    uint64_t after = NativeNanos(actor.oracle);
    Require(result == 0 && time.tv_nsec >= 0 && time.tv_nsec < 1000000000 &&
            Nanos(time) >= 0 && before <= static_cast<uint64_t>(Nanos(time)) &&
            static_cast<uint64_t>(Nanos(time)) <= after, "CPU sample matches independently owned native target");
    Sleep(1);
  }
}
void Dead(clockid_t clock) {
  timespec value{123, 456};
  errno = 0;
  Require(clock_gettime(clock, &value) == -1 && errno == EINVAL &&
          value.tv_sec == 123 && value.tv_nsec == 456, "retired gettime leaves output unchanged");
  errno = 0;
  Require(peer->resolution(clock, &value) == -1 && errno == EINVAL &&
          value.tv_sec == 123 && value.tv_nsec == 456, "retired cross-image getres leaves output unchanged");
}
void StopJoin(Actor& actor, pthread_t thread, bool peer_join = false) {
  Require(SetEvent(actor.release), "release target");
  void* result = nullptr;
  Require((peer_join ? peer->join(thread, &result) : pthread_join(thread, &result)) == 0 &&
          result == reinterpret_cast<void*>(uintptr_t{7}), "cross-image join observes actual result");
  Retire(actor);
}

struct UnattachedObserver {
  Actor actor;
  HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE attach = CreateEventW(nullptr, TRUE, FALSE, nullptr);
};
DWORD WINAPI AttachObserver(void* raw) {
  auto& observer = *static_cast<UnattachedObserver*>(raw);
  Require(SetEvent(observer.started), "pre-created observer has entered user code");
  Require(WaitForSingleObject(observer.attach, 15000) == WAIT_OBJECT_0, "observer attachment deadline");
  Run(&observer.actor);
  return 259;
}

void EndedBeforeNativeExit() {
  // Run immediately after the one warm-up pthread. Its record is consumed by
  // the target, so an early detach retirement supplies the observer's reuse.
  // The observer already runs before gating DLL_THREAD_DETACH: creating a new
  // kernel thread while this controlled callback holds the loader lock would
  // deadlock its initial DLL notifications and would not test this race.
  UnattachedObserver observer; observer.actor.check_cleanup = true;
  HANDLE observer_thread = CreateThread(nullptr, 0, AttachObserver, &observer, 0, nullptr);
  Require(observer_thread && WaitForSingleObject(observer.started, 15000) == WAIT_OBJECT_0,
          "unattached observer preexists final-exit gate");
  clock_exit_gate gate{CreateEventW(nullptr, TRUE, FALSE, nullptr),
                       CreateEventW(nullptr, TRUE, FALSE, nullptr), 0};
  Require(gate.reached && gate.release, "pre-created exit gate events");
  Actor target; target.check_cleanup = true; target.exit_gate = &gate;
  pthread_t thread;
  Require(pthread_create(&thread, nullptr, Run, &target) == 0, "create late-detach target");
  clockid_t saved = Acquire(target);
  void* original_record = lookup_record(target.id);
  Require(original_record != nullptr, "target owns a registered record");
  Require(SetEvent(target.release) && WaitForSingleObject(gate.reached, 15000) == WAIT_OBJECT_0,
          "target wrapper ended before supplier final TLS callback");
  Require(WaitForSingleObject(target.oracle, 0) == WAIT_TIMEOUT && target.cleanup_calls == 1,
          "key cleanup finished while native thread is still in DLL teardown");
  Require(pthread_detach(thread) == 0, "nonblocking detach after wrapper ended publication");
  Require(SetEvent(observer.attach), "force another live record allocation after late detach");
  clockid_t observer_clock = Acquire(observer.actor);
  void* observer_record = lookup_record(observer.actor.id);
  Require(lookup_record(target.id) == original_record && observer_record != nullptr &&
          observer_record != original_record, "pending TLS record cannot be pooled or reused");
  clockid_t again = 0;
  Require(pthread_getcpuclockid(target.id, &again) == 0 && again == saved,
          "late-detached but not-yet-terminated target keeps its identity");
  Require(SetEvent(gate.release), "release controlled final TLS boundary");
  Retire(target);
  Require(gate.error == 0 && target.cleanup_calls == 1 && lookup_record(target.id) == nullptr,
          "final TLS callback retires detached record exactly once");
  Dead(saved);
  Brackets(observer.actor, observer_clock);
  Require(lookup_record(observer.actor.id) == observer_record,
          "old thread teardown leaves replacement identity and record intact");
  Require(SetEvent(observer.actor.release) && WaitForSingleObject(observer_thread, 15000) == WAIT_OBJECT_0,
          "observer actually exits after gated target");
  Retire(observer.actor); Close(observer_thread); Dead(observer_clock);
  Require(observer.actor.cleanup_calls == 1 && lookup_record(observer.actor.id) == nullptr,
          "observer key cleanup and record retirement remain intact");
  Close(observer.started); Close(observer.attach); Close(gate.reached); Close(gate.release);
  Pass("deterministic ended-before-native-exit detach preserves pending TLS and prevents record reuse");
}

void ExternalExitStress() {
  auto cycle = [](int mode) {
    Actor actor; actor.pthread_exit = (mode & 1) != 0; actor.check_cleanup = true;
    HANDLE thread = CreateThread(nullptr, 0, NativeRun, &actor, 0, nullptr);
    Require(thread != nullptr, "create repeated external target");
    clockid_t clock = Acquire(actor);
    if (mode & 2) Require(pthread_detach(actor.id) == 0, "detach live external attachment");
    Require(SetEvent(actor.release) && WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0,
            "repeated external target actually exits");
    DWORD code = 0;
    Require(GetExitCodeThread(thread, &code) && code == 259, "repeated native exit259");
    Require(actor.cleanup_calls == 1 && lookup_record(actor.id) == nullptr,
            "native return/pthread_exit auto-retire record and key destructor exactly once");
    Retire(actor); Close(thread); Dead(clock);
  };
  for (int mode = 0; mode < 4; ++mode) cycle(mode);
  DWORD baseline = Handles();
  for (int mode = 0; mode < 4; ++mode) {
    for (int i = 0; i < 64; ++i) {
      cycle(mode);
      Require(Handles() == baseline, "external exit cycle leaks no handles after warm-up");
    }
    std::printf("MEASURE external_exit_mode=%d iterations=64 handles_baseline=%lu handles_final=%lu retired_records=64\n",
        mode, static_cast<unsigned long>(baseline), static_cast<unsigned long>(Handles()));
  }
  Pass("256 external native-return/pthread_exit cycles, joinable and detached, exact handle balance and record retirement");
}

void Basic(HMODULE provider) {
  using getid_fn = int (*)(pthread_t, clockid_t*);
  using read_fn = int (*)(clockid_t, timespec*);
  auto symbol_id = reinterpret_cast<getid_fn>(GetProcAddress(provider, "pthread_getcpuclockid"));
  auto symbol_read = reinterpret_cast<read_fn>(GetProcAddress(provider, "clock_gettime"));
  Require(symbol_id && symbol_read, "public literal symbols exist in shared supplier");
  Actor busy, idle;
  busy.busy = true;
  pthread_t busy_thread, idle_thread;
  Require(peer->create(&busy_thread, nullptr, Run, &busy) == 0 &&
          pthread_create(&idle_thread, nullptr, Run, &idle) == 0, "create one peer and one host target");
  clockid_t busy_clock = Acquire(busy), idle_clock = Acquire(idle), literal = 0;
  Require(busy_clock != idle_clock && symbol_id(busy_thread, &literal) == 0 && literal == busy_clock,
          "literal-symbol acquisition identifies requested peer");
  uint64_t busy_before = NativeNanos(busy.oracle), idle_before = NativeNanos(idle.oracle);
  timespec caller_before{}, caller_after{};
  Require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &caller_before) == 0, "fixed caller clock before sleep");
  Sleep(250);
  Require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &caller_after) == 0, "fixed caller clock after sleep");
  uint64_t busy_delta = NativeNanos(busy.oracle) - busy_before;
  uint64_t idle_delta = NativeNanos(idle.oracle) - idle_before;
  Require(busy_delta > 1000000 && busy_delta > idle_delta &&
          busy_delta > static_cast<uint64_t>(Nanos(caller_after) - Nanos(caller_before)),
          "sleeping caller measures busy peer rather than itself");
  std::printf("MEASURE busy_ns=%llu idle_ns=%llu caller_ns=%lld\n",
      static_cast<unsigned long long>(busy_delta), static_cast<unsigned long long>(idle_delta),
      static_cast<long long>(Nanos(caller_after) - Nanos(caller_before)));
  Brackets(busy, busy_clock); Brackets(idle, idle_clock);
  timespec sample{}, resolution{};
  uint64_t before = NativeNanos(busy.oracle);
  Require(symbol_read(busy_clock, &sample) == 0, "literal exported reader accepts same clock");
  uint64_t after = NativeNanos(busy.oracle);
  Require(before <= static_cast<uint64_t>(Nanos(sample)) && static_cast<uint64_t>(Nanos(sample)) <= after,
          "literal symbol matches native bracket");
  timespec fixed_resolution{};
  Require(peer->resolution(busy_clock, &resolution) == 0 &&
          clock_getres(CLOCK_THREAD_CPUTIME_ID, &fixed_resolution) == 0 &&
          Nanos(resolution) > 0 && Nanos(resolution) == Nanos(fixed_resolution) &&
          clock_getres(busy_clock, nullptr) == 0, "same backend resolution as fixed CPU clock and null output");
  _timespec32 narrow{};
  before = NativeNanos(busy.oracle);
  Require(clock_gettime32(busy_clock, &narrow) == 0, "public time32 ABI accepts the same target clock");
  after = NativeNanos(busy.oracle);
  uint64_t narrow_ns = uint64_t{static_cast<unsigned>(narrow.tv_sec)} * 1000000000 + narrow.tv_nsec;
  Require(before <= narrow_ns && narrow_ns <= after && clock_getres32(busy_clock, nullptr) == 0,
          "time32 target bracket and null resolution");
  errno = 0;
  Require(symbol_read(busy_clock, nullptr) == -1 && errno == EFAULT, "null read output returns EFAULT");
  errno = 0;
  Require(clock_gettime32(busy_clock, nullptr) == -1 && errno == EFAULT, "time32 null read output returns EFAULT");
  clockid_t unchanged = 123;
  errno = EDOM;
  Require(pthread_getcpuclockid(0, &unchanged) == ESRCH && unchanged == 123 && errno == EDOM &&
          pthread_getcpuclockid(busy_thread, nullptr) == EINVAL, "invalid IDs/outputs never masquerade as caller");
  void* owned = nullptr;
  unsigned long long generation = 0;
  Require(gesher_winpthread_dup_cpu_handle_v1(busy_thread, &owned, &generation) == 0 &&
          owned != nullptr && generation != 0 && GetThreadId(owned) == GetThreadId(busy.oracle),
          "private capability is independently owned and identifies the live target");
  void* untouched_handle = reinterpret_cast<void*>(uintptr_t{123});
  unsigned long long untouched_generation = 456;
  Require(gesher_winpthread_dup_cpu_handle_v1(0, &untouched_handle, &untouched_generation) == ESRCH &&
          untouched_handle == reinterpret_cast<void*>(uintptr_t{123}) && untouched_generation == 456,
          "invalid capability lookup preserves both outputs");
  DWORD handles = Handles();
  LARGE_INTEGER start, finish, frequency;
  Require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "measurement clock");
  for (int i = 0; i < 10000; ++i) {
    clockid_t again = 0;
    Require(pthread_getcpuclockid(busy_thread, &again) == 0 && again == busy_clock, "repeated acquisition shares clock");
  }
  Require(QueryPerformanceCounter(&finish), "measurement finish");
  Require(Handles() == handles, "repeated acquisition retains no additional handles");
  std::printf("MEASURE acquire_iterations=10000 total_ns=%.0f handles=%lu\n",
      (finish.QuadPart - start.QuadPart) * 1e9 / frequency.QuadPart, static_cast<unsigned long>(handles));
  StopJoin(busy, busy_thread); StopJoin(idle, idle_thread, true);
  Require(WaitForSingleObject(owned, 0) == WAIT_OBJECT_0, "owned capability survives provider target retirement");
  (void)NativeNanos(owned); Close(owned);
  Dead(busy_clock); Dead(idle_clock);
  Pass("cross-DLL/literal-symbol CPU brackets, errno, nulls and acquisition lifetime");
}

void NativeAndStd() {
  for (int i = 0; i < 2; ++i) {
    Actor actor; actor.busy = true; actor.pthread_exit = i != 0;
    HANDLE thread = CreateThread(nullptr, 0, NativeRun, &actor, 0, nullptr);
    Require(thread != nullptr, "create native Win32 target");
    clockid_t clock = Acquire(actor); Brackets(actor, clock);
    Require(SetEvent(actor.release) && WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0, "native target exits");
    DWORD exit = 0;
    Require(GetExitCodeThread(thread, &exit) && exit == 259, "real exit259 is not live");
    Close(thread); Retire(actor); Dead(clock);
  }
  Actor actor; actor.busy = true;
  std::thread thread([&] { Run(&actor); });
  clockid_t clock = Acquire(actor);
  Require(GetThreadId(thread.native_handle()) == GetThreadId(actor.oracle), "std thread native handle identifies oracle");
  Brackets(actor, clock);
  Require(SetEvent(actor.release), "release std thread"); thread.join(); Retire(actor); Dead(clock);
  Pass("CreateThread, native pthread_exit(259), and std::thread targets");
}

void CapacityAndReuse() {
  std::array<Actor, 257> actors;
  std::array<pthread_t, 257> threads{};
  std::array<clockid_t, 256> clocks{};
  for (size_t i = 0; i < actors.size(); ++i) {
    pthread_attr_t attr;
    Require(pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, 65536) == 0,
            "bounded capacity target stack");
    Require(pthread_create(&threads[i], &attr, Run, &actors[i]) == 0 && pthread_attr_destroy(&attr) == 0,
            "create capacity target");
    Ready(actors[i]);
    if (i < clocks.size()) clocks[i] = Acquire(actors[i]);
  }
  clockid_t unchanged = 123;
  errno = EDOM;
  Require(pthread_getcpuclockid(actors[256].id, &unchanged) == EAGAIN && unchanged == 123 && errno == EDOM,
          "257th live clock returns EAGAIN with untouched output");
  Require(pthread_getcpuclockid(actors[0].id, &unchanged) == 0 && unchanged == clocks[0], "cached acquisition at capacity");
  for (size_t i = 0; i < actors.size(); ++i) StopJoin(actors[i], threads[i]);
  for (clockid_t clock : clocks) Dead(clock);
  for (int i = 0; i < 64; ++i) {
    Actor replacement;
    pthread_t thread;
    Require(pthread_create(&thread, nullptr, Run, &replacement) == 0, "create replacement target");
    clockid_t fresh = Acquire(replacement);
    for (clockid_t retired : clocks) Require(fresh != retired, "replacement never reuses saved clock ID");
    StopJoin(replacement, thread); Dead(fresh);
  }
  Pass("256 live slots, exhaustion preservation, exit reaping and 64 replacement generations");
}

void RacesAndCancellation() {
  for (int round = 0; round < 32; ++round) {
    Actor actor;
    pthread_t target;
    Require(pthread_create(&target, nullptr, Run, &actor) == 0, "create race target");
    clockid_t saved = Acquire(actor);
    std::atomic<bool> stopped{false};
    std::array<std::thread, 4> readers;
    for (auto& reader : readers) reader = std::thread([&] {
      while (!stopped.load()) {
        clockid_t value = 123;
        int result = peer->clock_id(actor.id, &value);
        Require((result == 0 && value == saved) || (result == ESRCH && value == 123), "acquire races with actual retirement");
        timespec time{123, 456};
        errno = 0;
        result = clock_gettime(saved, &time);
        Require(result == 0 || (result == -1 && errno == EINVAL && time.tv_sec == 123 && time.tv_nsec == 456),
                "read races preserve lifetime and failure output");
      }
    });
    if (round % 2) Require(pthread_detach(target) == 0, "detach live target under query contention");
    Require(SetEvent(actor.release), "retire race target");
    if (round % 2 == 0) Require(pthread_join(target, nullptr) == 0, "join race target");
    Require(WaitForSingleObject(actor.oracle, 15000) == WAIT_OBJECT_0, "racing target actually exited");
    stopped = true;
    for (auto& reader : readers) reader.join();
    Retire(actor); Dead(saved);
  }
  Actor canceled; canceled.test_cancel = true;
  pthread_t thread;
  Require(pthread_create(&thread, nullptr, Run, &canceled) == 0, "create cancel target");
  clockid_t clock = Acquire(canceled);
  Require(pthread_cancel(thread) == 0, "request real pthread cancellation");
  void* result = nullptr;
  Require(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED, "real cancellation return");
  Retire(canceled); Dead(clock);
  for (int i = 0; i < 128; ++i) {
    Actor actor;
    pthread_attr_t attr; pthread_t thread_id;
    Require(pthread_attr_init(&attr) == 0 && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
            pthread_create(&thread_id, &attr, Run, &actor) == 0 && pthread_attr_destroy(&attr) == 0,
            "create detached target");
    clockid_t detached = Acquire(actor);
    Require(SetEvent(actor.release), "release detached target");
    Retire(actor); Dead(detached);
  }
  Pass("four-reader exit/detach/join races, cancellation and 128 detached creations");
}
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE module = LoadLibraryExW(L"clock-peer.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  Require(module != nullptr, "load independently compiled peer DLL");
  auto entry = reinterpret_cast<clock_peer_entry>(GetProcAddress(module, "clock_peer_v1"));
  Require(entry != nullptr && (peer = entry()) != nullptr && peer->size == sizeof(*peer), "peer ABI");
  HMODULE provider = GetModuleHandleW(L"libwinpthread-1.dll");
  Require(provider != nullptr && peer->self == pthread_self && peer->clock_id == pthread_getcpuclockid,
          "host and peer import one shared pthread/clock provider");
  wchar_t provider_path[32768], host_path[32768];
  Require(GetModuleFileNameW(provider, provider_path, 32768) && GetModuleFileNameW(nullptr, host_path, 32768), "loaded module paths");
  std::wstring expected(host_path); expected.resize(expected.find_last_of(L"\\/") + 1); expected += L"libwinpthread-1.dll";
  Require(_wcsicmp(expected.c_str(), provider_path) == 0, "loaded supplier is the verified adjacent DLL");
  std::printf("PROVIDER one shared adjacent libwinpthread-1.dll\n");
  lookup_record = reinterpret_cast<lookup_record_fn>(GetProcAddress(provider, "__pth_gpointer_locked"));
  Require(lookup_record != nullptr, "read-only supplier registry lookup for lifetime regression");
  Require(pthread_self() == peer->self(), "caller cross-image identity");
  Require(pthread_key_create(&cleanup_key, Cleanup) == 0, "create lifecycle cleanup key");
  // Warm genuine provider TLS/cleanup locks before measuring persistent handles.
  Actor warm; warm.check_cleanup = true; pthread_t warm_id;
  Require(pthread_create(&warm_id, nullptr, Run, &warm) == 0, "warm target");
  Ready(warm); StopJoin(warm, warm_id);
  DWORD baseline = Handles();
  EndedBeforeNativeExit();
  Basic(provider); NativeAndStd(); ExternalExitStress(); CapacityAndReuse(); RacesAndCancellation();
  DWORD final = Handles();
  std::printf("MEASURE handles_baseline=%lu handles_final=%lu\n", static_cast<unsigned long>(baseline), static_cast<unsigned long>(final));
  Require(final <= baseline + 4, "real clock/supplier handles reclaimed within original baseline bound");
  Require(pthread_key_delete(cleanup_key) == 0, "delete lifecycle cleanup key");
  clockid_t self;
  Require(pthread_getcpuclockid(pthread_self(), &self) == 0 && FreeLibrary(module), "unload peer after all its targets exit");
  timespec value{};
  Require(clock_gettime(self, &value) == 0 && clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0 &&
          clock_gettime(CLOCK_MONOTONIC, &value) == 0, "surviving shared provider and original fixed clocks after peer unload");
  Pass("component completed; product adoption and ARM64 native runtime remain separate");
  return 0;
}
