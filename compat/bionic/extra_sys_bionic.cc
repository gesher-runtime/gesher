// gesher bionic shim: system-level functions beyond the core files
// (docs/native-libs.md, ADR 0057): POSIX semaphores, scheduling parameters,
// pthread spinlocks/barriers and the timed and attribute forms the core lacks,
// resource usage, utime/utimes, clock_nanosleep, sysinfo, prctl, getrandom,
// memfd_create, madvise, dup3, tgkill, flock and lockf. Registered (from
// RegisterExtra) before the core files, so their definitions win.

#include "extra_bionic.h"

#include <gesher/bionic_shim.h>

#include <windows.h>
// bcrypt.h needs windows.h first.
#include <bcrypt.h>
#define PSAPI_VERSION 2
#include <psapi.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gesher::bionic {

namespace {

#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)

template <class Fn>
Fn Shim(const char* name) {
  return reinterpret_cast<Fn>(LookupBionicSymbol(name));
}

std::atomic<uint32_t>* Word(void* p, int i = 0) {
  return reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uint32_t*>(p) + i);
}

int64_t NowRealtimeNs() {
  LinuxTimespec ts = RealtimeNow();
  return ts.tv_sec * 1000000000 + ts.tv_nsec;
}
int64_t NowMonotonicNs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<int64_t>((static_cast<__int128>(now.QuadPart) * 1000000000) / freq.QuadPart);
}
// Milliseconds until an absolute time on `clock` (0 realtime, else monotonic);
// -1 if it has passed.
int64_t MsUntil(const LinuxTimespec* abs, int clock) {
  int64_t now = clock == 0 ? NowRealtimeNs() : NowMonotonicNs();
  int64_t then = abs->tv_sec * 1000000000 + abs->tv_nsec;
  if (then <= now) return -1;
  return (then - now + 999999) / 1000000;
}

// --- POSIX semaphores on bionic's sem_t (16 bytes; word 0 is the count) ---------

int BIONIC_SYSV SemInit(void* s, int, unsigned v) {
  Word(s)->store(v);
  return 0;
}
int BIONIC_SYSV SemDestroy(void*) { return 0; }
int BIONIC_SYSV SemPost(void* s) {
  Word(s)->fetch_add(1);
  WakeByAddressSingle(Word(s));
  return 0;
}
bool SemTake(void* s) {
  uint32_t v = Word(s)->load();
  while (v > 0) {
    if (Word(s)->compare_exchange_weak(v, v - 1)) return true;
  }
  return false;
}
int BIONIC_SYSV SemTrywait(void* s) {
  if (SemTake(s)) return 0;
  errno = kEAGAIN;
  return -1;
}
// Waits until the count is positive or `abs` passes on `clock` (no `abs`: forever).
int SemWaitUntil(void* s, const LinuxTimespec* abs, int clock) {
  for (;;) {
    if (SemTake(s)) return 0;
    DeliverPendingSignals();
    DWORD ms = INFINITE;
    if (abs != nullptr) {
      if (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000) return errno = kEINVAL, -1;
      int64_t left = MsUntil(abs, clock);
      if (left < 0) return errno = kETIMEDOUT, -1;
      ms = static_cast<DWORD>(left > 0x7ffffffe ? 0x7ffffffe : left);
    }
    uint32_t zero = 0;
    WaitOnAddress(Word(s), &zero, 4, ms);
  }
}
int BIONIC_SYSV SemWait(void* s) { return SemWaitUntil(s, nullptr, 0); }
int BIONIC_SYSV SemTimedwait(void* s, const LinuxTimespec* abs) { return SemWaitUntil(s, abs, 0); }
int BIONIC_SYSV SemTimedwaitMonotonic(void* s, const LinuxTimespec* abs) { return SemWaitUntil(s, abs, 1); }
int BIONIC_SYSV SemClockwait(void* s, int clock, const LinuxTimespec* abs) {
  return SemWaitUntil(s, abs, clock == 0 ? 0 : 1);
}
int BIONIC_SYSV SemGetvalue(void* s, int* v) {
  *v = static_cast<int>(Word(s)->load());
  return 0;
}
// Named semaphores: bionic has none either.
void* BIONIC_SYSV SemOpen(const char*, int) {
  errno = kENOSYS;
  return nullptr;  // SEM_FAILED
}
int BIONIC_SYSV SemNamed(void*) {
  errno = kENOSYS;
  return -1;
}

// --- scheduling ---------------------------------------------------------------------

int BIONIC_SYSV SchedPriorityMin(int policy) { return policy == 1 || policy == 2 ? 1 : 0; }  // FIFO/RR
int BIONIC_SYSV SchedPriorityMax(int policy) { return policy == 1 || policy == 2 ? 99 : 0; }
int BIONIC_SYSV SchedGetparam(int, int* param) {
  if (param) *param = 0;
  return 0;
}
int BIONIC_SYSV SchedSetparam(int, const int*) { return 0; }
int BIONIC_SYSV SchedGetscheduler(int) { return 0; }  // SCHED_OTHER
int BIONIC_SYSV SchedSetscheduler(int, int policy, const int*) {
  if (policy != 0 && policy != 3 && policy != 5) return errno = kEPERM, -1;  // OTHER, BATCH, IDLE
  return 0;
}
int BIONIC_SYSV SchedRrGetInterval(int, LinuxTimespec* ts) {
  if (ts) *ts = {0, 100000000};
  return 0;
}
int ProcessorCount() {
  DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return n == 0 ? 1 : static_cast<int>(n);
}
int BIONIC_SYSV SchedGetaffinity(int, size_t size, uint8_t* set) {
  if (set == nullptr) return errno = kEFAULT, -1;
  std::memset(set, 0, size);
  int n = ProcessorCount();
  for (int i = 0; i < n && static_cast<size_t>(i / 8) < size; ++i) set[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
  return 0;
}
int BIONIC_SYSV SchedSetaffinity(int, size_t, const uint8_t*) { return 0; }
int BIONIC_SYSV SchedCpucount(size_t size, const uint8_t* set) {
  int c = 0;
  for (size_t i = 0; i < size; ++i) c += __builtin_popcount(set[i]);
  return c;
}
void* BIONIC_SYSV SchedCpualloc(size_t count) { return std::calloc((count + 63) / 64, 8); }
void BIONIC_SYSV SchedCpufree(void* p) { std::free(p); }

// --- pthread additions ----------------------------------------------------------------

int BIONIC_SYSV PthreadGetschedparam(uint64_t, int* policy, int* param) {
  if (policy) *policy = 0;
  if (param) *param = 0;
  return 0;
}
int BIONIC_SYSV PthreadSetschedparam(uint64_t, int, const int*) { return 0; }
int BIONIC_SYSV PthreadSetschedprio(uint64_t, int) { return 0; }
int BIONIC_SYSV PthreadAttrGetschedpolicy(const bionic_pthread_attr_t* a, int* p) {
  *p = a->sched_policy;
  return 0;
}
int BIONIC_SYSV PthreadAttrGetscope(const bionic_pthread_attr_t*, int* scope) {
  *scope = 0;  // PTHREAD_SCOPE_SYSTEM
  return 0;
}
int BIONIC_SYSV PthreadAttrGetinheritsched(const bionic_pthread_attr_t*, int* v) {
  *v = 0;  // PTHREAD_INHERIT_SCHED
  return 0;
}
int BIONIC_SYSV PthreadAttrSetinheritsched(bionic_pthread_attr_t*, int) { return 0; }
int BIONIC_SYSV PthreadAttrSetstack(bionic_pthread_attr_t* a, void* base, size_t size) {
  if (size < 16384) return kEINVAL;
  a->stack_base = base;
  a->stack_size = size;
  return 0;
}
int BIONIC_SYSV PthreadGetnameNp(uint64_t thread, char* buf, size_t size) {
  if (buf == nullptr || size == 0) return kERANGE;
  buf[0] = 0;
  if (thread == PthreadSelfId()) {
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name != nullptr) {
      WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, static_cast<int>(size), nullptr, nullptr);
      buf[size - 1] = 0;
      LocalFree(name);
    }
  }
  return 0;
}
int BIONIC_SYSV PthreadGetcpuclockid(uint64_t, int* clock) {
  *clock = 3;  // CLOCK_THREAD_CPUTIME_ID (the calling thread's)
  return 0;
}
int BIONIC_SYSV PthreadMutexattrGetpshared(const int64_t*, int* v) {
  *v = 0;
  return 0;
}
int BIONIC_SYSV PthreadMutexattrSetprotocol(int64_t*, int) { return 0; }
int BIONIC_SYSV PthreadMutexattrGetprotocol(const int64_t*, int* v) {
  *v = 0;
  return 0;
}
int BIONIC_SYSV PthreadCondattrGetclock(const int64_t* a, int* clock) {
  *clock = static_cast<int>(*a);  // pthread_bionic.cc keeps the clock id in it
  return 0;
}
int BIONIC_SYSV PthreadAttrPsharedGet(const void*, int* v) {
  *v = 0;
  return 0;
}
int BIONIC_SYSV PthreadAttrPsharedSet(void*, int) { return 0; }
int BIONIC_SYSV PthreadRwlockattrInit(int64_t* a) {
  *a = 0;
  return 0;
}
int BIONIC_SYSV PthreadRwlockattrDestroy(int64_t*) { return 0; }
int BIONIC_SYSV PthreadRwlockattrGetkind(const int64_t*, int* v) {
  *v = 0;
  return 0;
}

// The timed forms the core lacks: retry the try form until the deadline.
using TryFn = int(BIONIC_SYSV*)(void*);
int TimedRetry(const char* try_name, void* obj, const LinuxTimespec* abs, int clock) {
  TryFn try_fn = Shim<TryFn>(try_name);
  if (try_fn == nullptr) return kENOSYS;
  if (abs != nullptr && (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000)) return kEINVAL;
  for (int spins = 0;; ++spins) {
    int r = try_fn(obj);
    if (r != kEBUSY) return r;
    if (abs != nullptr && MsUntil(abs, clock) < 0) return kETIMEDOUT;
    if (spins < 64) {
      SwitchToThread();
    } else {
      Sleep(1);
    }
  }
}
int BIONIC_SYSV RwlockTimedrd(void* rw, const LinuxTimespec* abs) {
  return TimedRetry("pthread_rwlock_tryrdlock", rw, abs, 0);
}
int BIONIC_SYSV RwlockTimedwr(void* rw, const LinuxTimespec* abs) {
  return TimedRetry("pthread_rwlock_trywrlock", rw, abs, 0);
}
int BIONIC_SYSV RwlockTimedrdMono(void* rw, const LinuxTimespec* abs) {
  return TimedRetry("pthread_rwlock_tryrdlock", rw, abs, 1);
}
int BIONIC_SYSV RwlockTimedwrMono(void* rw, const LinuxTimespec* abs) {
  return TimedRetry("pthread_rwlock_trywrlock", rw, abs, 1);
}
int BIONIC_SYSV RwlockClockrd(void* rw, int clock, const LinuxTimespec* abs) {
  return TimedRetry("pthread_rwlock_tryrdlock", rw, abs, clock == 0 ? 0 : 1);
}
int BIONIC_SYSV RwlockClockwr(void* rw, int clock, const LinuxTimespec* abs) {
  return TimedRetry("pthread_rwlock_trywrlock", rw, abs, clock == 0 ? 0 : 1);
}
int BIONIC_SYSV MutexTimedlockMono(void* m, const LinuxTimespec* abs) {
  return TimedRetry("pthread_mutex_trylock", m, abs, 1);
}
int BIONIC_SYSV MutexClocklock(void* m, int clock, const LinuxTimespec* abs) {
  return TimedRetry("pthread_mutex_trylock", m, abs, clock == 0 ? 0 : 1);
}
int BIONIC_SYSV CondClockwait(void* c, void* m, int clock, const LinuxTimespec* abs) {
  using Timed = int(BIONIC_SYSV*)(void*, void*, const LinuxTimespec*);
  Timed fn = Shim<Timed>(clock == 0 ? "pthread_cond_timedwait" : "pthread_cond_timedwait_monotonic_np");
  return fn != nullptr ? fn(c, m, abs) : kENOSYS;
}

// Spinlocks (8 bytes): word 0 is 1 while held.
int BIONIC_SYSV SpinInit(void* l, int) {
  Word(l)->store(0);
  return 0;
}
int BIONIC_SYSV SpinDestroy(void*) { return 0; }
int BIONIC_SYSV SpinTrylock(void* l) {
  uint32_t zero = 0;
  return Word(l)->compare_exchange_strong(zero, 1, std::memory_order_acquire) ? 0 : kEBUSY;
}
int BIONIC_SYSV SpinLock(void* l) {
  for (int spins = 0; SpinTrylock(l) != 0; ++spins) {
    if (spins > 100) SwitchToThread();
  }
  return 0;
}
int BIONIC_SYSV SpinUnlock(void* l) {
  Word(l)->store(0, std::memory_order_release);
  return 0;
}

// Barriers (32 bytes): word 0 the count, 1 the waiters so far, 2 the generation.
int BIONIC_SYSV BarrierInit(void* b, const void*, unsigned count) {
  if (count == 0) return kEINVAL;
  Word(b, 0)->store(count);
  Word(b, 1)->store(0);
  Word(b, 2)->store(0);
  return 0;
}
int BIONIC_SYSV BarrierDestroy(void*) { return 0; }
int BIONIC_SYSV BarrierWait(void* b) {
  uint32_t gen = Word(b, 2)->load();
  if (Word(b, 1)->fetch_add(1) + 1 == Word(b, 0)->load()) {
    Word(b, 1)->store(0);
    Word(b, 2)->fetch_add(1);
    WakeByAddressAll(Word(b, 2));
    return -1;  // PTHREAD_BARRIER_SERIAL_THREAD
  }
  while (Word(b, 2)->load() == gen) WaitOnAddress(Word(b, 2), &gen, 4, INFINITE);
  return 0;
}
int BIONIC_SYSV BarrierattrInit(int* a) {
  *a = 0;
  return 0;
}
int BIONIC_SYSV BarrierattrDestroy(int*) { return 0; }

// --- resource usage and time --------------------------------------------------------

int64_t FileTime100ns(FILETIME f) {
  return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
struct LinuxTimeval {
  int64_t tv_sec, tv_usec;
};
struct LinuxRusage {
  LinuxTimeval ru_utime, ru_stime;
  int64_t ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock,
      ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
static_assert(sizeof(LinuxRusage) == 144, "struct rusage");
LinuxTimeval Tv(int64_t t100) { return {t100 / 10000000, (t100 % 10000000) / 10}; }
int BIONIC_SYSV Getrusage(int who, LinuxRusage* ru) {
  if (ru == nullptr) return errno = kEFAULT, -1;
  std::memset(ru, 0, sizeof(*ru));
  FILETIME c, e, k, u;
  if (who == 1) {  // RUSAGE_THREAD
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return errno = kEINVAL, -1;
  } else if (who == 0) {  // RUSAGE_SELF
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return errno = kEINVAL, -1;
  } else if (who == -1) {  // RUSAGE_CHILDREN: none tracked
    return 0;
  } else {
    return errno = kEINVAL, -1;
  }
  ru->ru_utime = Tv(FileTime100ns(u));
  ru->ru_stime = Tv(FileTime100ns(k));
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    ru->ru_maxrss = static_cast<int64_t>(pmc.PeakWorkingSetSize / 1024);
    ru->ru_minflt = pmc.PageFaultCount;
  }
  return 0;
}
struct LinuxTms {
  int64_t tms_utime, tms_stime, tms_cutime, tms_cstime;
};
int64_t BIONIC_SYSV Times(LinuxTms* t) {
  if (t != nullptr) {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    t->tms_utime = FileTime100ns(u) / 100000;  // clock ticks of 10 ms (_SC_CLK_TCK 100)
    t->tms_stime = FileTime100ns(k) / 100000;
    t->tms_cutime = t->tms_cstime = 0;
  }
  return static_cast<int64_t>(GetTickCount64() / 10);
}

using UtimensatFn = int(BIONIC_SYSV*)(int, const char*, const LinuxTimespec*, int);
using FutimensFn = int(BIONIC_SYSV*)(int, const LinuxTimespec*);
constexpr int kAtFdcwd = -100;
constexpr int kAtSymlinkNofollow = 0x100;
int SetTimes(int dirfd, const char* path, const LinuxTimespec* ts, int flags) {
  static auto fn = Shim<UtimensatFn>("utimensat");
  if (fn == nullptr) return errno = kENOSYS, -1;
  return fn(dirfd, path, ts, flags);
}
int BIONIC_SYSV Utime(const char* path, const int64_t* times) {  // struct utimbuf {actime, modtime}
  if (times == nullptr) return SetTimes(kAtFdcwd, path, nullptr, 0);
  LinuxTimespec ts[2] = {{times[0], 0}, {times[1], 0}};
  return SetTimes(kAtFdcwd, path, ts, 0);
}
int TimesFromTimeval(const LinuxTimeval* tv, LinuxTimespec ts[2]) {
  for (int i = 0; i < 2; ++i) {
    if (tv[i].tv_usec < 0 || tv[i].tv_usec >= 1000000) return -1;
    ts[i] = {tv[i].tv_sec, tv[i].tv_usec * 1000};
  }
  return 0;
}
int BIONIC_SYSV Utimes(const char* path, const LinuxTimeval* tv) {
  if (tv == nullptr) return SetTimes(kAtFdcwd, path, nullptr, 0);
  LinuxTimespec ts[2];
  if (TimesFromTimeval(tv, ts) != 0) return errno = kEINVAL, -1;
  return SetTimes(kAtFdcwd, path, ts, 0);
}
int BIONIC_SYSV Lutimes(const char* path, const LinuxTimeval* tv) {
  if (tv == nullptr) return SetTimes(kAtFdcwd, path, nullptr, kAtSymlinkNofollow);
  LinuxTimespec ts[2];
  if (TimesFromTimeval(tv, ts) != 0) return errno = kEINVAL, -1;
  return SetTimes(kAtFdcwd, path, ts, kAtSymlinkNofollow);
}
int BIONIC_SYSV Futimesat(int dirfd, const char* path, const LinuxTimeval* tv) {
  if (tv == nullptr) return SetTimes(dirfd, path, nullptr, 0);
  LinuxTimespec ts[2];
  if (TimesFromTimeval(tv, ts) != 0) return errno = kEINVAL, -1;
  return SetTimes(dirfd, path, ts, 0);
}
int BIONIC_SYSV Futimes(int fd, const LinuxTimeval* tv) {
  static auto fn = Shim<FutimensFn>("futimens");
  if (fn == nullptr) return errno = kENOSYS, -1;
  if (tv == nullptr) return fn(fd, nullptr);
  LinuxTimespec ts[2];
  if (TimesFromTimeval(tv, ts) != 0) return errno = kEINVAL, -1;
  return fn(fd, ts);
}

// clock_nanosleep returns the error number (not -1/errno).
int BIONIC_SYSV ClockNanosleep(int clock, int flags, const LinuxTimespec* req, LinuxTimespec* rem) {
  if (req == nullptr || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) return kEINVAL;
  if (clock == 3) return kEINVAL;  // CLOCK_THREAD_CPUTIME_ID
  int64_t ms;
  if ((flags & 1) != 0) {  // TIMER_ABSTIME
    ms = MsUntil(req, clock == 0 || clock == 5 ? 0 : 1);
    if (ms < 0) return 0;
  } else {
    ms = req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000;
  }
  DeliverPendingSignals();
  Sleep(static_cast<DWORD>(ms > 0x7ffffffe ? 0x7ffffffe : ms));
  DeliverPendingSignals();
  if (rem != nullptr && (flags & 1) == 0) *rem = {0, 0};
  return 0;
}
int BIONIC_SYSV ClockGetcpuclockid(int pid, int* clock) {
  if (pid != 0 && pid != static_cast<int>(GetCurrentProcessId())) return kESRCH;
  *clock = 2;  // CLOCK_PROCESS_CPUTIME_ID
  return 0;
}
int BIONIC_SYSV ClockSettime(int, const LinuxTimespec*) { return errno = kEPERM, -1; }
int BIONIC_SYSV Settimeofday(const void*, const void*) { return errno = kEPERM, -1; }

int BIONIC_SYSV Sysinfo(void* out) {
  if (out == nullptr) return errno = kEFAULT, -1;
  // Fill only Linux's 112 bytes.
  struct {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t pad2;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    uint32_t pad3;
  } s{};
  static_assert(sizeof(s) == 112, "struct sysinfo");
  s.uptime = static_cast<int64_t>(GetTickCount64() / 1000);
  MEMORYSTATUSEX m;
  m.dwLength = sizeof(m);
  if (GlobalMemoryStatusEx(&m)) {
    s.totalram = m.ullTotalPhys;
    s.freeram = m.ullAvailPhys;
    s.totalswap = m.ullTotalPageFile > m.ullTotalPhys ? m.ullTotalPageFile - m.ullTotalPhys : 0;
    s.freeswap = m.ullAvailPageFile > m.ullAvailPhys ? m.ullAvailPageFile - m.ullAvailPhys : 0;
  }
  s.procs = 1;
  s.mem_unit = 1;
  std::memcpy(out, &s, sizeof(s));
  return 0;
}

// --- Linux system calls as functions ------------------------------------------------

thread_local char t_thread_name[16];
int BIONIC_SYSV Prctl(int option, uint64_t a2, uint64_t, uint64_t, uint64_t) {
  switch (option) {
    case 15: {  // PR_SET_NAME
      const char* name = reinterpret_cast<const char*>(a2);
      if (name == nullptr) return errno = kEFAULT, -1;
      std::snprintf(t_thread_name, sizeof(t_thread_name), "%s", name);
      wchar_t w[16];
      if (MultiByteToWideChar(CP_UTF8, 0, t_thread_name, -1, w, 16) > 0) {
        SetThreadDescription(GetCurrentThread(), w);
      }
      return 0;
    }
    case 16:  // PR_GET_NAME
      if (a2 == 0) return errno = kEFAULT, -1;
      std::memcpy(reinterpret_cast<char*>(a2), t_thread_name, sizeof(t_thread_name));
      return 0;
    case 3:   // PR_GET_DUMPABLE
      return 1;
    case 4:   // PR_SET_DUMPABLE
    case 1:   // PR_SET_PDEATHSIG
    case 29:  // PR_SET_TIMERSLACK
    case 38:  // PR_SET_NO_NEW_PRIVS
    case 0x53564d41:  // PR_SET_VMA (anonymous mapping names)
      return 0;
    case 2:   // PR_GET_PDEATHSIG
      if (a2 != 0) *reinterpret_cast<int*>(a2) = 0;
      return 0;
    case 39:  // PR_GET_NO_NEW_PRIVS
    case 23:  // PR_CAPBSET_READ
      return 0;
    default:
      return errno = kEINVAL, -1;
  }
}
int64_t BIONIC_SYSV Getrandom(void* buf, size_t n, unsigned) {
  if (n > 0x7fffffff) n = 0x7fffffff;
  if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return errno = kEINVAL, -1;
  }
  return static_cast<int64_t>(n);
}
int BIONIC_SYSV Getentropy(void* buf, size_t n) {
  if (n > 256) return errno = 5, -1;  // EIO, as bionic
  return Getrandom(buf, n, 0) < 0 ? -1 : 0;
}
int BIONIC_SYSV MemfdCreate(const char* name, unsigned flags) { return memfd_create(name, flags); }
int BIONIC_SYSV Madvise(void* addr, size_t len, int advice) { return madvise(addr, len, advice); }
int BIONIC_SYSV Msync(void* addr, size_t len, int flags) { return msync(addr, len, flags); }
int BIONIC_SYSV Zero2(const void*, size_t) { return 0; }
int BIONIC_SYSV Zero1(int) { return 0; }
int BIONIC_SYSV Zero0() { return 0; }
int BIONIC_SYSV Mincore(void*, size_t len, unsigned char* vec) {
  for (size_t i = 0; i < (len + 4095) / 4096; ++i) vec[i] = 1;
  return 0;
}
int BIONIC_SYSV PosixMadvise(void*, size_t, int) { return 0; }
int BIONIC_SYSV Dup3(int oldfd, int newfd, int flags) {
  if (oldfd == newfd) return errno = kEINVAL, -1;
  if ((flags & ~O_CLOEXEC) != 0) return errno = kEINVAL, -1;
  int r = dup2(oldfd, newfd);
  if (r >= 0 && (flags & O_CLOEXEC) != 0) fcntl(r, F_SETFD, FD_CLOEXEC);
  return r;
}
int BIONIC_SYSV Tgkill(int, int tid, int sig) {
  if (tid != static_cast<int>(GetCurrentThreadId())) return errno = kESRCH, -1;
  static auto raise_fn = Shim<int(BIONIC_SYSV*)(int)>("raise");
  return raise_fn != nullptr ? raise_fn(sig) : (errno = kENOSYS, -1);
}
int BIONIC_SYSV Tkill(int tid, int sig) { return Tgkill(0, tid, sig); }
int BIONIC_SYSV Flock(int fd, int op) { return flock(fd, op); }
// lockf: record locks, like the shim's fcntl ones, are not modelled (granted).
int BIONIC_SYSV Lockf(int fd, int cmd, int64_t) {
  if (fcntl(fd, F_GETFD) < 0) return errno = kEBADF, -1;
  return cmd >= 0 && cmd <= 3 ? 0 : (errno = kEINVAL, -1);
}
int BIONIC_SYSV PosixFadvise(int fd, int64_t off, int64_t len, int advice) {
  return posix_fadvise(fd, static_cast<off_t>(off), static_cast<off_t>(len), advice);
}
int BIONIC_SYSV PosixFallocate(int fd, int64_t off, int64_t len) {
  return posix_fallocate(fd, static_cast<off_t>(off), static_cast<off_t>(len));
}
int BIONIC_SYSV Fallocate(int fd, int mode, int64_t off, int64_t len) {
  if (mode != 0) return errno = kEOPNOTSUPP, -1;
  int r = PosixFallocate(fd, off, len);
  if (r != 0) return errno = r, -1;
  return 0;
}
void BIONIC_SYSV Sync() {}
int BIONIC_SYSV Getpid() { return static_cast<int>(GetCurrentProcessId()); }
int BIONIC_SYSV Getpgid(int pid) { return pid == 0 ? Getpid() : pid; }
int BIONIC_SYSV Setsid() { return Getpid(); }
int BIONIC_SYSV Getgroups(int size, uint32_t* list) {
  if (size == 0) return 1;
  list[0] = static_cast<uint32_t>(getgid());
  return 1;
}
int BIONIC_SYSV Setgroups(size_t, const uint32_t*) { return errno = kEPERM, -1; }
int BIONIC_SYSV Getresuid(uint32_t* r, uint32_t* e, uint32_t* s) {
  *r = *e = *s = static_cast<uint32_t>(getuid());
  return 0;
}
int BIONIC_SYSV Getresgid(uint32_t* r, uint32_t* e, uint32_t* s) {
  *r = *e = *s = static_cast<uint32_t>(getgid());
  return 0;
}
char* BIONIC_SYSV Ttyname(int fd) {
  static char name[] = "/dev/tty";
  if (!isatty(fd)) return errno = 25, nullptr;  // ENOTTY
  return name;
}
int BIONIC_SYSV TtynameR(int fd, char* buf, size_t size) {
  if (!isatty(fd)) return 25;
  if (size < 9) return kERANGE;
  std::strcpy(buf, "/dev/tty");
  return 0;
}
int BIONIC_SYSV Tcgetattr(int fd, void* t) {
  if (!isatty(fd)) return errno = 25, -1;
  std::memset(t, 0, 60);  // struct termios: a raw terminal
  return 0;
}
int BIONIC_SYSV Tcsetattr(int fd, int, const void*) {
  if (!isatty(fd)) return errno = 25, -1;
  return 0;
}
// inotify: gesher-base's (Linux's event layout, an fd poll/epoll wait on).
int BIONIC_SYSV InotifyInit() { return inotify_init(); }
int BIONIC_SYSV InotifyInit1(int flags) { return inotify_init1(flags); }
int BIONIC_SYSV InotifyAddWatch(int fd, const char* path, uint32_t mask) {
  return inotify_add_watch(fd, path, mask);
}
int BIONIC_SYSV InotifyRmWatch(int fd, int wd) { return inotify_rm_watch(fd, wd); }
int BIONIC_SYSV Getdomainname(char* buf, size_t size) {
  if (size == 0) return errno = kEINVAL, -1;
  std::snprintf(buf, size, "(none)");
  return 0;
}
int BIONIC_SYSV Sethostname(const char*, size_t) { return errno = kEPERM, -1; }

}  // namespace

// --- setjmp/longjmp (x86-64, System V) ---------------------------------------
// bionic's x86-64 jmp_buf is 11 longs; the layout is the shim's own, since
// only these functions read it: rbx, rbp, r12-r15, the stack pointer after
// the return, the return address, whether the signal mask was saved, the
// mask. setjmp saves the mask (bionic's does), _setjmp does not, sigsetjmp
// when asked; every longjmp restores it if it was saved. (The arm64
// translator has guest versions: guest_libc.cc.)
extern "C" void BIONIC_SYSV gesher_bionic_jmp_savemask(uint64_t* env) {
  static auto fn = Shim<int(BIONIC_SYSV*)(int, const uint64_t*, uint64_t*)>("sigprocmask");
  env[9] = 0;
  if (fn != nullptr) fn(0 /* SIG_BLOCK */, nullptr, &env[9]);
}
extern "C" void BIONIC_SYSV gesher_bionic_jmp_restoremask(uint64_t* env) {
  static auto fn = Shim<int(BIONIC_SYSV*)(int, const uint64_t*, uint64_t*)>("sigprocmask");
  if (fn != nullptr) fn(2 /* SIG_SETMASK */, &env[9], nullptr);
}
asm(".text\n"
    ".globl gesher_bionic_setjmp\n"
    ".p2align 4\n"
    "gesher_bionic_setjmp:\n"
    "  movl $1, %esi\n"
    "  jmp gesher_bionic_sigsetjmp\n"
    ".globl gesher_bionic__setjmp\n"
    ".p2align 4\n"
    "gesher_bionic__setjmp:\n"
    "  xorl %esi, %esi\n"
    ".globl gesher_bionic_sigsetjmp\n"
    "gesher_bionic_sigsetjmp:\n"
    "  movq %rbx, 0(%rdi)\n"
    "  movq %rbp, 8(%rdi)\n"
    "  movq %r12, 16(%rdi)\n"
    "  movq %r13, 24(%rdi)\n"
    "  movq %r14, 32(%rdi)\n"
    "  movq %r15, 40(%rdi)\n"
    "  leaq 8(%rsp), %rdx\n"
    "  movq %rdx, 48(%rdi)\n"
    "  movq (%rsp), %rdx\n"
    "  movq %rdx, 56(%rdi)\n"
    "  movslq %esi, %rsi\n"
    "  movq %rsi, 64(%rdi)\n"
    "  testq %rsi, %rsi\n"
    "  jz 1f\n"
    "  subq $8, %rsp\n"
    "  call gesher_bionic_jmp_savemask\n"
    "  addq $8, %rsp\n"
    "1:\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl gesher_bionic_longjmp\n"
    ".p2align 4\n"
    "gesher_bionic_longjmp:\n"
    "  cmpq $0, 64(%rdi)\n"
    "  je 1f\n"
    "  pushq %rdi\n"
    "  pushq %rsi\n"
    "  subq $8, %rsp\n"
    "  call gesher_bionic_jmp_restoremask\n"
    "  addq $8, %rsp\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "1:\n"
    "  movq 0(%rdi), %rbx\n"
    "  movq 8(%rdi), %rbp\n"
    "  movq 16(%rdi), %r12\n"
    "  movq 24(%rdi), %r13\n"
    "  movq 32(%rdi), %r14\n"
    "  movq 40(%rdi), %r15\n"
    "  movq 48(%rdi), %rsp\n"
    "  movl %esi, %eax\n"
    "  testl %eax, %eax\n"
    "  jnz 2f\n"
    "  movl $1, %eax\n"
    "2:\n"
    "  jmp *56(%rdi)\n");
extern "C" int gesher_bionic_setjmp();
extern "C" int gesher_bionic__setjmp();
extern "C" int gesher_bionic_sigsetjmp();
extern "C" void gesher_bionic_longjmp();

void RegisterExtraSys(SymbolTable& t) {
  t["setjmp"] = reinterpret_cast<void*>(&gesher_bionic_setjmp);
  t["_setjmp"] = reinterpret_cast<void*>(&gesher_bionic__setjmp);
  t["sigsetjmp"] = reinterpret_cast<void*>(&gesher_bionic_sigsetjmp);
  t["longjmp"] = reinterpret_cast<void*>(&gesher_bionic_longjmp);
  t["_longjmp"] = reinterpret_cast<void*>(&gesher_bionic_longjmp);
  t["siglongjmp"] = reinterpret_cast<void*>(&gesher_bionic_longjmp);
  E("sem_init", SemInit);
  E("sem_destroy", SemDestroy);
  E("sem_post", SemPost);
  E("sem_wait", SemWait);
  E("sem_trywait", SemTrywait);
  E("sem_timedwait", SemTimedwait);
  E("sem_timedwait_monotonic_np", SemTimedwaitMonotonic);
  E("sem_clockwait", SemClockwait);
  E("sem_getvalue", SemGetvalue);
  E("sem_open", SemOpen);
  E("sem_close", SemNamed);
  E("sem_unlink", SemNamed);
  E("sched_get_priority_min", SchedPriorityMin);
  E("sched_get_priority_max", SchedPriorityMax);
  E("sched_getparam", SchedGetparam);
  E("sched_setparam", SchedSetparam);
  E("sched_getscheduler", SchedGetscheduler);
  E("sched_setscheduler", SchedSetscheduler);
  E("sched_rr_get_interval", SchedRrGetInterval);
  E("sched_getaffinity", SchedGetaffinity);
  E("sched_setaffinity", SchedSetaffinity);
  E("__sched_cpucount", SchedCpucount);
  E("__sched_cpualloc", SchedCpualloc);
  E("__sched_cpufree", SchedCpufree);
  E("pthread_getschedparam", PthreadGetschedparam);
  E("pthread_setschedparam", PthreadSetschedparam);
  E("pthread_setschedprio", PthreadSetschedprio);
  E("pthread_attr_getschedpolicy", PthreadAttrGetschedpolicy);
  E("pthread_attr_getscope", PthreadAttrGetscope);
  E("pthread_attr_getinheritsched", PthreadAttrGetinheritsched);
  E("pthread_attr_setinheritsched", PthreadAttrSetinheritsched);
  E("pthread_attr_setstack", PthreadAttrSetstack);
  E("pthread_getname_np", PthreadGetnameNp);
  E("pthread_getcpuclockid", PthreadGetcpuclockid);
  E("pthread_mutexattr_getpshared", PthreadMutexattrGetpshared);
  E("pthread_mutexattr_setprotocol", PthreadMutexattrSetprotocol);
  E("pthread_mutexattr_getprotocol", PthreadMutexattrGetprotocol);
  E("pthread_condattr_getclock", PthreadCondattrGetclock);
  E("pthread_condattr_getpshared", PthreadAttrPsharedGet);
  E("pthread_condattr_setpshared", PthreadAttrPsharedSet);
  E("pthread_rwlockattr_init", PthreadRwlockattrInit);
  E("pthread_rwlockattr_destroy", PthreadRwlockattrDestroy);
  E("pthread_rwlockattr_getpshared", PthreadAttrPsharedGet);
  E("pthread_rwlockattr_setpshared", PthreadAttrPsharedSet);
  E("pthread_rwlockattr_getkind_np", PthreadRwlockattrGetkind);
  E("pthread_rwlockattr_setkind_np", PthreadAttrPsharedSet);
  E("pthread_rwlock_timedrdlock", RwlockTimedrd);
  E("pthread_rwlock_timedwrlock", RwlockTimedwr);
  E("pthread_rwlock_timedrdlock_monotonic_np", RwlockTimedrdMono);
  E("pthread_rwlock_timedwrlock_monotonic_np", RwlockTimedwrMono);
  E("pthread_rwlock_clockrdlock", RwlockClockrd);
  E("pthread_rwlock_clockwrlock", RwlockClockwr);
  E("pthread_mutex_timedlock_monotonic_np", MutexTimedlockMono);
  E("pthread_mutex_clocklock", MutexClocklock);
  E("pthread_cond_clockwait", CondClockwait);
  E("pthread_spin_init", SpinInit);
  E("pthread_spin_destroy", SpinDestroy);
  E("pthread_spin_lock", SpinLock);
  E("pthread_spin_trylock", SpinTrylock);
  E("pthread_spin_unlock", SpinUnlock);
  E("pthread_barrier_init", BarrierInit);
  E("pthread_barrier_destroy", BarrierDestroy);
  E("pthread_barrier_wait", BarrierWait);
  E("pthread_barrierattr_init", BarrierattrInit);
  E("pthread_barrierattr_destroy", BarrierattrDestroy);
  E("pthread_barrierattr_getpshared", PthreadAttrPsharedGet);
  E("pthread_barrierattr_setpshared", PthreadAttrPsharedSet);
  E("getrusage", Getrusage);
  E("times", Times);
  E("utime", Utime);
  E("utimes", Utimes);
  E("lutimes", Lutimes);
  E("futimes", Futimes);
  E("futimesat", Futimesat);
  E("clock_nanosleep", ClockNanosleep);
  E("clock_getcpuclockid", ClockGetcpuclockid);
  E("clock_settime", ClockSettime);
  E("settimeofday", Settimeofday);
  E("sysinfo", Sysinfo);
  E("prctl", Prctl);
  E("getrandom", Getrandom);
  E("getentropy", Getentropy);
  E("memfd_create", MemfdCreate);
  E("madvise", Madvise);
  E("posix_madvise", PosixMadvise);
  E("msync", Msync);
  E("mlock", Zero2);
  E("munlock", Zero2);
  E("mlock2", Zero2);
  E("mlockall", Zero1);
  E("munlockall", Zero0);
  E("mincore", Mincore);
  E("dup3", Dup3);
  E("tgkill", Tgkill);
  E("tkill", Tkill);
  E("flock", Flock);
  E("lockf", Lockf);
  E("lockf64", Lockf);
  E("posix_fadvise", PosixFadvise);
  E("posix_fadvise64", PosixFadvise);
  E("posix_fallocate", PosixFallocate);
  E("posix_fallocate64", PosixFallocate);
  E("fallocate", Fallocate);
  E("fallocate64", Fallocate);
  E("sync", Sync);
  E("syncfs", Zero1);
  E("getpgrp", Getpid);
  E("getpgid", Getpgid);
  E("getsid", Getpgid);
  E("setsid", Setsid);
  E("setpgid", Zero2);
  E("getgroups", Getgroups);
  E("setgroups", Setgroups);
  E("getresuid", Getresuid);
  E("getresgid", Getresgid);
  E("ttyname", Ttyname);
  E("ttyname_r", TtynameR);
  E("tcgetattr", Tcgetattr);
  E("tcsetattr", Tcsetattr);
  E("inotify_init", InotifyInit);
  E("inotify_init1", InotifyInit1);
  E("inotify_add_watch", InotifyAddWatch);
  E("inotify_rm_watch", InotifyRmWatch);
  E("getdomainname", Getdomainname);
  E("sethostname", Sethostname);
}

}  // namespace gesher::bionic
