// Real host target controls. This executable is built and run only in CI.
#include <pthread.h>
#include <time.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <memory>

namespace {
std::atomic<int> fixture_owned_handles{0};
void Require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL %s error=%lu errno=%d\n", what, GetLastError(), errno); std::abort(); }
}
void CountHandles(const char* stage) {
  DWORD count = 0;
  Require(GetProcessHandleCount(GetCurrentProcess(), &count), "diagnostic process handles");
  std::printf("HOST_HANDLE_STAGE stage=%s process=%lu fixture-owned=%d\n", stage, count,
              fixture_owned_handles.load());
}
void CloseOwned(HANDLE handle) {
  Require(CloseHandle(handle), "fixture owned close");
  Require(--fixture_owned_handles >= 0, "fixture ownership accounting");
}
struct Target {
  std::atomic<bool> ready{false}, stop{false}, busy{true};
  pthread_t identity{};
  HANDLE oracle = nullptr;
  std::atomic<unsigned long long> work{0};
};
void* Worker(void* arg) {
  auto& t = *static_cast<Target*>(arg);
  t.identity = pthread_self();
  Require(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                          &t.oracle, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0), "oracle duplicate");
  ++fixture_owned_handles;
  t.ready = true;
  while (!t.stop) {
    pthread_testcancel();
    if (t.busy) t.work.fetch_add(1, std::memory_order_relaxed);
    else Sleep(1);
  }
  return nullptr;
}
DWORD WINAPI NativeWorker(void* arg) { Worker(arg); return 259; }
void Await(Target& t) {
  ULONGLONG until = GetTickCount64() + 5000;
  while (!t.ready && GetTickCount64() < until) Sleep(1);
  Require(t.ready, "target readiness");
}
unsigned long long Ticks(HANDLE h) {
  FILETIME c, e, k, u;
  Require(GetThreadTimes(h, &c, &e, &k, &u), "oracle time");
  return ((static_cast<unsigned long long>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
         ((static_cast<unsigned long long>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}
unsigned long long Sample(clockid_t id) {
  timespec t{};
  Require(clock_gettime(id, &t) == 0, "target sample");
  return static_cast<unsigned long long>(t.tv_sec) * 10000000 + t.tv_nsec / 100;
}
clockid_t Check(Target& t, const char* name) {
  Await(t);
  clockid_t id = 123, same = 456;
  Require(pthread_getcpuclockid(t.identity, &id) == 0, "target acquire");
  Require(pthread_getcpuclockid(t.identity, &same) == 0 && same == id, "dedup clock");
  auto before = Ticks(t.oracle), sample = Sample(id), after = Ticks(t.oracle);
  Require(before <= sample && sample <= after, "independent target bracket");
  auto start = Sample(id);
  Sleep(250);
  Require(Sample(id) > start && t.work != 0, "busy target while caller sleeps");
  t.busy = false;
  Sleep(50);
  start = Sample(id);
  Sleep(200);
  Require(Sample(id) - start <= 500000, "idle target cpu bound");
  timespec res{};
  Require(clock_getres(id, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 100, "clock resolution");
  std::printf("PASS host-authority %s target-bracket busy idle dedup\n", name);
  return id;
}
void Dead(Target& t, clockid_t id) {
  Require(WaitForSingleObject(t.oracle, 5000) == WAIT_OBJECT_0, "real target exit");
  clockid_t unchanged = 123;
  Require(pthread_getcpuclockid(t.identity, &unchanged) == ESRCH && unchanged == 123, "dead identity unchanged");
  timespec unchanged_time{42, 43};
  Require(clock_gettime(id, &unchanged_time) == -1 && errno == EINVAL &&
          unchanged_time.tv_sec == 42 && unchanged_time.tv_nsec == 43, "dead clock unchanged");
  CloseOwned(t.oracle);
}

pthread_key_t exit_key;
std::atomic<int> exit_destructors{0};
std::atomic<bool> hold_destructor{false}, destructor_entered{false}, release_destructor{false};
void ExitDestructor(void*) {
  ++exit_destructors;
  if (hold_destructor) {
    destructor_entered = true;
    while (!release_destructor) Sleep(0);
  }
}
DWORD WINAPI ExplicitExitWorker(void* arg) {
  auto& t = *static_cast<Target*>(arg);
  t.identity = pthread_self();
  Require(pthread_setspecific(exit_key, &t) == 0, "external set key");
  Require(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                          &t.oracle, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0), "external oracle");
  ++fixture_owned_handles;
  t.ready = true;
  while (!t.stop) Sleep(0);
  pthread_exit(nullptr);
  return 0;
}
void ExternalExitStress() {
  Require(pthread_key_create(&exit_key, ExitDestructor) == 0, "external key create");
  for (int i = 0; i != 256; ++i) {
    Target t;
    HANDLE thread = CreateThread(nullptr, 0, ExplicitExitWorker, &t, 0, nullptr);
    Require(thread != nullptr, "external explicit create"); ++fixture_owned_handles;
    Await(t);
    clockid_t saved = 0;
    Require(pthread_getcpuclockid(t.identity, &saved) == 0, "external clock acquire");
    if (i % 4 == 0) Require(pthread_detach(t.identity) == 0, "external live detach");
    hold_destructor = i % 4 == 1;
    destructor_entered = false;
    release_destructor = false;
    t.stop = true;
    if (i % 4 == 1) {
      ULONGLONG deadline = GetTickCount64() + 5000;
      while (!destructor_entered && GetTickCount64() < deadline) Sleep(1);
      Require(destructor_entered, "external destructor barrier");
      Require(pthread_detach(t.identity) == 0, "external detach during destructor");
      release_destructor = true;
    }
    Require(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "external real exit");
    if (i % 4 == 2) Require(pthread_join(t.identity, nullptr) == 0, "external join");
    if (i % 4 == 3) Require(pthread_detach(t.identity) == 0, "external ended detach");
    Require(exit_destructors == i + 1, "external destructor exactly once");
    Dead(t, saved);
    CloseOwned(thread);
  }
  Require(pthread_key_delete(exit_key) == 0, "external key delete");
  std::puts("PASS host-authority external-explicit-exit live-detach destructor-barrier join ended-detach key-destructor reuse stale-clock");
  CountHandles("external-exit-retired");
}
void Stress() {
  CountHandles("stress-entry");
  // Every exhausted slot holds a genuine, still-live distinct target.
  constexpr int count = 257;
  auto targets = std::make_unique<Target[]>(count);
  std::vector<pthread_t> threads(count);
  std::vector<clockid_t> clocks(count, 123);
  for (int i = 0; i != count; ++i) {
    targets[i].busy = false;
    Require(pthread_create(&threads[i], nullptr, Worker, &targets[i]) == 0, "capacity real create");
    Await(targets[i]);
    int rc = pthread_getcpuclockid(targets[i].identity, &clocks[i]);
    Require(i < 256 ? rc == 0 : rc == EAGAIN && clocks[i] == 123, "real target slot capacity");
    if (i < 256) Require(Sample(clocks[i]) <= Ticks(targets[i].oracle), "capacity retained oracle");
  }
  for (int i = 0; i != count; ++i) targets[i].stop = true;
  for (int i = 0; i != count; ++i) {
    Require(pthread_join(threads[i], nullptr) == 0, "capacity join");
    if (i < 256) Dead(targets[i], clocks[i]);
    else CloseOwned(targets[i].oracle);
  }
  CountHandles("capacity-retired");
  { Target t; pthread_t th; t.busy = false;
    Require(pthread_create(&th, nullptr, Worker, &t) == 0, "replacement create"); Await(t);
    timespec untouched{42, 43};
    Require(clock_gettime(clocks[0], &untouched) == -1 && errno == EINVAL &&
            untouched.tv_sec == 42 && untouched.tv_nsec == 43, "saved clock cannot alias replacement");
    clockid_t fresh;
    Require(pthread_getcpuclockid(t.identity, &fresh) == 0 && fresh != clocks[0], "replacement distinct clock");
    t.stop = true; Require(pthread_join(th, nullptr) == 0, "replacement join"); Dead(t, fresh); }
  std::puts("PASS host-authority 256-live-slots exhaustion-output reclaim saved-clock-nonalias");
  CountHandles("replacement-retired");

  for (int round = 0; round != 64; ++round) {
    Target t; pthread_t th; t.busy = false;
    Require(pthread_create(&th, nullptr, Worker, &t) == 0, "race target create"); Await(t);
    clockid_t saved;
    Require(pthread_getcpuclockid(t.identity, &saved) == 0, "race initial clock");
    std::atomic<int> started{0};
    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;
    for (int i = 0; i != 4; ++i) readers.emplace_back([&] {
      bool announced = false;
      while (!stop_readers) {
        clockid_t id = 123;
        int rc = pthread_getcpuclockid(t.identity, &id);
        Require(rc == 0 || (rc == ESRCH && id == 123 && t.stop), "racing acquire result");
        if (!announced) { Require(rc == 0, "real live reader acquisition"); ++started; announced = true; }
        if (rc == 0) {
          Require(id == saved, "race generation dedup");
          timespec ts{};
          int qr = clock_gettime(id, &ts);
          Require(qr == 0 || (qr == -1 && errno == EINVAL && t.stop), "racing query result");
        }
      }
    });
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (started != 4 && GetTickCount64() < deadline) Sleep(1);
    Require(started == 4, "real racing acquisitions started");
    if (round % 2 == 0) Require(pthread_detach(th) == 0, "race live detach");
    t.stop = true;
    if (round % 2 != 0) Require(pthread_join(th, nullptr) == 0, "race join");
    Require(WaitForSingleObject(t.oracle, 5000) == WAIT_OBJECT_0, "race actual exit");
    stop_readers = true;
    for (auto& reader : readers) reader.join();
    Dead(t, saved);
    std::printf("HOST_RACE_ROUND row=%d\n", round);
    CountHandles("race-retired");
  }
  std::puts("PASS host-authority 64-rounds four-readers concurrent-detach-join-exit");

  { Target t; pthread_t th; t.busy = false;
    Require(pthread_create(&th, nullptr, Worker, &t) == 0, "cancel target create"); Await(t);
    clockid_t saved; Require(pthread_getcpuclockid(t.identity, &saved) == 0, "cancel acquire");
    Require(pthread_cancel(th) == 0, "real cancel request"); void* result = nullptr;
    Require(pthread_join(th, &result) == 0 && result == PTHREAD_CANCELED, "real cancellation join result");
    Dead(t, saved); }
  std::puts("PASS host-authority real-cancel target-clock-retired");
  CountHandles("cancel-retired");

  auto rapid = std::make_unique<Target[]>(1024);
  pthread_attr_t attr;
  Require(pthread_attr_init(&attr) == 0 && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0, "rapid detached attr");
  for (int i = 0; i != 1024; ++i) {
    rapid[i].stop = true; pthread_t th;
    Require(pthread_create(&th, &attr, Worker, &rapid[i]) == 0, "rapid detached create");
  }
  for (int i = 0; i != 1024; ++i) {
    Await(rapid[i]);
    Require(WaitForSingleObject(rapid[i].oracle, 5000) == WAIT_OBJECT_0, "rapid real exit");
    CloseOwned(rapid[i].oracle);
  }
  pthread_attr_destroy(&attr);
  std::puts("PASS host-authority 1024-rapid-detached real-exit-and-reclamation");
  CountHandles("rapid-detached-retired");
}
}
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  clockid_t unchanged = 123;
  Require(pthread_getcpuclockid(static_cast<pthread_t>(-1), &unchanged) == ESRCH && unchanged == 123, "invalid identity");
  Require(pthread_getcpuclockid(pthread_self(), nullptr) == EINVAL, "null output");
  Require(Sample(CLOCK_THREAD_CPUTIME_ID) <= Sample(CLOCK_PROCESS_CPUTIME_ID), "original fixed clocks");
  DWORD baseline = 0, final_count = 0;
  Require(GetProcessHandleCount(GetCurrentProcess(), &baseline), "handle baseline");
  CountHandles("baseline");
  for (int i = 0; i != 32; ++i) {
    Target t;
    pthread_t thread;
    Require(pthread_create(&thread, nullptr, Worker, &t) == 0, "pthread create");
    Await(t);
    Require(thread == t.identity, "created target identity");
    clockid_t id = i == 0 ? Check(t, "pthread") : 0;
    if (i != 0) Require(pthread_getcpuclockid(thread, &id) == 0, "reused pool acquire");
    t.stop = true;
    Require(pthread_join(thread, nullptr) == 0, "real pthread join");
    Dead(t, id);
  }
  { Target t; std::thread th([&] { Worker(&t); }); Await(t);
    // This compiler's libc++ uses the Win32 thread backend: native_handle
    // is a HANDLE, whereas Worker obtained a genuine pthread_self identity.
    // Authenticate the native oracle while both objects are retained/live;
    // never reinterpret a kernel handle as a public pthread identity.
    HANDLE native = nullptr;
    Require(DuplicateHandle(GetCurrentProcess(), th.native_handle(), GetCurrentProcess(),
                            &native, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0), "std native oracle duplicate");
    ++fixture_owned_handles;
    DWORD tid = GetThreadId(native);
    Require(tid != 0 && tid == GetThreadId(t.oracle), "std native live target identity");
    FILETIME native_created, oracle_created, exited, kernel, user;
    Require(GetThreadTimes(native, &native_created, &exited, &kernel, &user) &&
            GetThreadTimes(t.oracle, &oracle_created, &exited, &kernel, &user) &&
            CompareFileTime(&native_created, &oracle_created) == 0, "std native target incarnation");
    auto id = Check(t, "std-thread");
    auto before = Ticks(native), sample = Sample(id), after = Ticks(native);
    Require(before <= sample && sample <= after, "std native owned oracle bracket");
    CloseOwned(native); t.stop = true; th.join(); Dead(t, id); }
  { Target t; pthread_attr_t attr; pthread_t th;
    Require(pthread_attr_init(&attr) == 0 && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0, "detached attr");
    Require(pthread_create(&th, &attr, Worker, &t) == 0, "detached create");
    auto id = Check(t, "detached-create"); t.stop = true; Dead(t, id); pthread_attr_destroy(&attr); }
  { Target t; pthread_t th;
    Require(pthread_create(&th, nullptr, Worker, &t) == 0, "detach create");
    Await(t); Require(pthread_detach(th) == 0, "live detach");
    auto id = Check(t, "detached-live"); t.stop = true; Dead(t, id); }
  { Target t; HANDLE th = CreateThread(nullptr, 0, NativeWorker, &t, 0, nullptr);
    Require(th != nullptr, "Win32 create"); ++fixture_owned_handles;
    auto id = Check(t, "Win32-self-attach");
    t.stop = true; Dead(t, id); DWORD exit = 0;
    Require(GetExitCodeThread(th, &exit) && exit == 259, "real exit 259"); CloseOwned(th); }
  ExternalExitStress();
  Stress();
  CountHandles("final-before-original-bound");
  Require(fixture_owned_handles == 0, "all fixture-owned handles closed");
  Require(GetProcessHandleCount(GetCurrentProcess(), &final_count) && final_count <= baseline + 8, "handle reclamation bound");
  std::printf("PASS host-authority dead reuse exit259 invalid fixed-clocks handle-count baseline=%lu final=%lu\n", baseline, final_count);
}
