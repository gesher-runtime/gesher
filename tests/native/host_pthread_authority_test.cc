// Real host target controls. This executable is built and run only in CI.
#include <pthread.h>
#include <time.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
void Require(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL %s error=%lu errno=%d\n", what, GetLastError(), errno); std::abort(); }
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
  t.ready = true;
  while (!t.stop) {
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
  CloseHandle(t.oracle);
}
}
int main() {
  clockid_t unchanged = 123;
  Require(pthread_getcpuclockid(static_cast<pthread_t>(-1), &unchanged) == ESRCH && unchanged == 123, "invalid identity");
  Require(pthread_getcpuclockid(pthread_self(), nullptr) == EINVAL, "null output");
  Require(Sample(CLOCK_THREAD_CPUTIME_ID) <= Sample(CLOCK_PROCESS_CPUTIME_ID), "original fixed clocks");
  DWORD baseline = 0, final_count = 0;
  Require(GetProcessHandleCount(GetCurrentProcess(), &baseline), "handle baseline");
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
    DWORD tid = GetThreadId(native);
    Require(tid != 0 && tid == GetThreadId(t.oracle), "std native live target identity");
    FILETIME native_created, oracle_created, exited, kernel, user;
    Require(GetThreadTimes(native, &native_created, &exited, &kernel, &user) &&
            GetThreadTimes(t.oracle, &oracle_created, &exited, &kernel, &user) &&
            CompareFileTime(&native_created, &oracle_created) == 0, "std native target incarnation");
    auto id = Check(t, "std-thread");
    auto before = Ticks(native), sample = Sample(id), after = Ticks(native);
    Require(before <= sample && sample <= after, "std native owned oracle bracket");
    CloseHandle(native); t.stop = true; th.join(); Dead(t, id); }
  { Target t; pthread_attr_t attr; pthread_t th;
    Require(pthread_attr_init(&attr) == 0 && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0, "detached attr");
    Require(pthread_create(&th, &attr, Worker, &t) == 0, "detached create");
    auto id = Check(t, "detached-create"); t.stop = true; Dead(t, id); pthread_attr_destroy(&attr); }
  { Target t; pthread_t th;
    Require(pthread_create(&th, nullptr, Worker, &t) == 0, "detach create");
    Await(t); Require(pthread_detach(th) == 0, "live detach");
    auto id = Check(t, "detached-live"); t.stop = true; Dead(t, id); }
  { Target t; HANDLE th = CreateThread(nullptr, 0, NativeWorker, &t, 0, nullptr);
    Require(th != nullptr, "Win32 create"); auto id = Check(t, "Win32-self-attach");
    t.stop = true; Dead(t, id); DWORD exit = 0;
    Require(GetExitCodeThread(th, &exit) && exit == 259, "real exit 259"); CloseHandle(th); }
  Require(GetProcessHandleCount(GetCurrentProcess(), &final_count) && final_count <= baseline + 8, "handle reclamation bound");
  std::printf("PASS host-authority dead reuse exit259 invalid fixed-clocks handle-count baseline=%lu final=%lu\n", baseline, final_count);
}
