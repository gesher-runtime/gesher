// gesher bionic shim: bionic's pthread ABI over Windows.
//
// The app's .so uses bionic's pthread types (abi_types.h) and ids, whose
// layouts differ from winpthreads', so these cannot forward to winpthreads;
// they are implemented directly on Windows primitives (threads, WaitOnAddress
// futexes on 32-bit words inside the bionic-sized objects). Bionic's static
// initializers are honoured: PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER
// and PTHREAD_RWLOCK_INITIALIZER are all-zero, and the recursive / error-check
// mutex initializers put the type in bits 14-15 of the first word.
//
// All entry points are System V (BIONIC_SYSV), as the .so calls them.

#include "shim_internal.h"
#include "pthread_cleanup.h"

#include <pthread.h>
#include <windows.h>

#include <atomic>
#include <array>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gesher/elf_loader.h>

namespace gesher::bionic {

namespace {

// --- futex helpers ---------------------------------------------------------

using Word = std::atomic<int32_t>;

Word* W(void* object, int index) { return reinterpret_cast<Word*>(object) + index; }

// Waits while *w == expected, up to `ms`. Returns false on timeout.
bool FutexWait(Word* w, int32_t expected, DWORD ms = INFINITE) {
  int32_t cmp = expected;
  if (WaitOnAddress(reinterpret_cast<volatile void*>(w), &cmp, sizeof(int32_t), ms)) return true;
  return GetLastError() != ERROR_TIMEOUT;
}
void FutexWake(Word* w, bool all) {
  if (all) {
    WakeByAddressAll(reinterpret_cast<void*>(w));
  } else {
    WakeByAddressSingle(reinterpret_cast<void*>(w));
  }
}

int32_t SelfTid() { return static_cast<int32_t>(GetCurrentThreadId()); }

// --- thread-exit work: key destructors and C++ thread_local destructors ----

constexpr int kKeysMax = 128;         // bionic's PTHREAD_KEYS_MAX
constexpr int kDestructorPasses = 4;  // PTHREAD_DESTRUCTOR_ITERATIONS

std::mutex g_keys_mutex;
void (BIONIC_SYSV* g_key_dtors[kKeysMax])(void*) = {};
bool g_key_used[kKeysMax] = {};
// A reused key slot must not expose another generation's thread-local value.
// Retire a slot at UINT64_MAX rather than letting its generation wrap.
uint64_t g_key_generations[kKeysMax] = {};
thread_local void* t_key_values[kKeysMax] = {};
thread_local uint64_t t_key_generations[kKeysMax] = {};

struct ThreadAtExit {
  void (BIONIC_SYSV* dtor)(void*);
  void* obj;
};
thread_local std::vector<ThreadAtExit>* t_thread_atexit = nullptr;
thread_local CleanupStack t_cleanups;

// C++ thread_local destructors, last registered first (as bionic).
void RunThreadAtExit() {
  if (t_thread_atexit == nullptr) return;
  while (!t_thread_atexit->empty()) {
    ThreadAtExit e = t_thread_atexit->back();
    t_thread_atexit->pop_back();
    e.dtor(e.obj);
  }
  delete t_thread_atexit;
  t_thread_atexit = nullptr;
}

// One round of key destructors: each key set when the round reaches it, once.
// Returns whether any destructor ran.
bool RunKeyRound() {
  bool any = false;
  for (int k = 0; k < kKeysMax; ++k) {
    void* value = t_key_values[k];
    if (value == nullptr) continue;
    void (BIONIC_SYSV* dtor)(void*) = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_keys_mutex);
      if (g_key_used[k] && t_key_generations[k] == g_key_generations[k]) {
        dtor = g_key_dtors[k];
      }
    }
    t_key_values[k] = nullptr;
    t_key_generations[k] = 0;
    if (dtor != nullptr) {
      dtor(value);
      any = true;
    }
  }
  return any;
}

void RunThreadExit() {
  t_cleanups.Run();
  RunThreadAtExit();
  for (int pass = 0; pass < kDestructorPasses; ++pass) {
    if (!RunKeyRound()) break;
  }
}

// --- threads ---------------------------------------------------------------

constexpr uint32_t kAttrDetached = 1;  // bionic PTHREAD_ATTR_FLAG_DETACHED
constexpr uint64_t kDefaultStack = 1024 * 1024;

bionic_pthread_attr_t DefaultThreadAttributes() {
  bionic_pthread_attr_t attr{};
  attr.stack_size = kDefaultStack;
  attr.guard_size = 4096;
  return attr;
}

bionic_pthread_attr_t CurrentThreadAttributes() {
  // Only the system-allocated stack interval is observed here. Guard and
  // scheduling fields retain this adapter's compatibility defaults: Windows'
  // moving guard page is not a separately allocated POSIX guard region, and
  // custom stack/guard/scheduling requests are not implemented by CreateThread.
  bionic_pthread_attr_t attr = DefaultThreadAttributes();
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  attr.stack_base = reinterpret_cast<void*>(low);
  attr.stack_size = high - low;
  return attr;
}

struct ThreadRecord {
  HANDLE handle = nullptr;
  void* retval = nullptr;
  int32_t tid = 0;
  bool detached = false;
  bionic_pthread_attr_t attributes{};
  bool attributes_ready = false;
  bionic_pthread_attr_t translated_attributes{};
  bool translated_attributes_ready = false;
};

std::mutex g_threads_mutex;
std::condition_variable g_thread_attributes_changed;
std::unordered_map<bionic_pthread_t, ThreadRecord*> g_threads;
// Host/ART-created threads can enter bionic without pthread_create. Keep
// their self identities authoritative too; reclaim terminated objects on
// subsequent identity/clock operations. No TID is reopened after exit.
struct ExternalThreadRecord {
  HANDLE handle = nullptr;
  bionic_pthread_attr_t attributes{};
  bionic_pthread_attr_t translated_attributes{};
  bool translated_attributes_ready = false;
};
std::unordered_map<bionic_pthread_t, ExternalThreadRecord> g_external_threads;
std::atomic<bionic_pthread_t> g_next_id{1};

thread_local bionic_pthread_t t_self = 0;
thread_local ThreadRecord* t_record = nullptr;

// Clock IDs are capabilities for one retained kernel thread object, never a
// Windows TID (which Windows can reuse). All access uses g_threads_mutex.
// Repeated acquisition shares a slot; dead slots close their handle on the
// next clock operation. Retired IDs are never assigned again.
struct CpuClock {
  bionic_pthread_t thread = 0;
  HANDLE handle = nullptr;
  int id = 0;
};
std::array<CpuClock, 256> g_cpu_clocks{};
int64_t g_next_cpu_clock = -65536;

void ReapCpuClocks() {
  for (auto it = g_external_threads.begin(); it != g_external_threads.end();) {
    if (it->second.handle != nullptr &&
        WaitForSingleObject(it->second.handle, 0) != WAIT_TIMEOUT) {
      CloseHandle(it->second.handle);
      it = g_external_threads.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& clock : g_cpu_clocks) {
    if (clock.handle != nullptr && WaitForSingleObject(clock.handle, 0) != WAIT_TIMEOUT) {
      CloseHandle(clock.handle);
      clock = {};
    }
  }
}

// A thread this shim did not start (ART's threads, Windows' and host
// libraries' callback threads) has no FinishThread. Its exit work runs from a
// winpthreads key instead, in the rounds winpthreads runs ART's key in: one
// round of these per round of those, as bionic runs all of a thread's keys
// together. A library's destructor that detaches the thread from the VM then
// runs before ART's key comes round again; ART's first call only warns and
// re-sets its key, its second aborts the process ("Native thread exited
// without calling DetachCurrentThread", Waze's attached threads).
pthread_key_t g_host_exit_key;
std::once_flag g_host_exit_once;
thread_local int t_host_exit_rounds = -1;  // -1: not armed on this thread

void HostExitRound(void*) {
  ++t_host_exit_rounds;
  t_cleanups.Run();
  RunThreadAtExit();
  RunKeyRound();
}

void ArmHostExit() {
  if (t_record != nullptr) return;
  if (t_host_exit_rounds < 0) {
    std::call_once(g_host_exit_once,
                   [] { pthread_key_create(&g_host_exit_key, HostExitRound); });
    t_host_exit_rounds = 0;
  } else if (t_host_exit_rounds == 0 || t_host_exit_rounds >= kDestructorPasses) {
    return;
  }
  // Armed, or set again while the thread exits: its destructor runs next round.
  pthread_setspecific(g_host_exit_key, &t_host_exit_rounds);
}

struct StartArg {
  void* (BIONIC_SYSV* start)(void*);
  void* arg;
  bionic_pthread_t id;
  ThreadRecord* record;
};

// Frees a detached thread's record when it ends.
void FinishThread(void* retval) {
  RunThreadExit();
  if (t_record == nullptr) return;
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  t_record->retval = retval;
  if (t_record->detached) {
    CloseHandle(t_record->handle);
    g_threads.erase(t_self);
    delete t_record;
  }
  t_record = nullptr;
}

DWORD WINAPI ThreadTrampoline(void* raw) {
  StartArg sa = *static_cast<StartArg*>(raw);
  delete static_cast<StartArg*>(raw);
  t_self = sa.id;
  t_record = sa.record;
  {
    // CreateThread can start before its creator has published the handle.
    // The same registry lock makes that publication and this snapshot ordered.
    // Publish before TLS initialization or any caller-supplied function runs.
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    t_record->attributes = CurrentThreadAttributes();
    t_record->attributes_ready = true;
  }
  g_thread_attributes_changed.notify_all();
  gesher::elf::SetupThreadTls();  // the thread's TLS block for loaded code
  void* ret = sa.start(sa.arg);
  FinishThread(ret);
  return 0;
}

bionic_pthread_t SelfId() {
  if (t_self == 0) {
    t_self = g_next_id.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    ReapCpuClocks();
    HANDLE retained = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &retained,
                    THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0);
    // A null entry records resource exhaustion without confusing this valid
    // pthread identity with an unknown one. Acquisition then returns EAGAIN.
    g_external_threads.emplace(t_self,
                               ExternalThreadRecord{retained, CurrentThreadAttributes()});
  }
  return t_self;
}

int ThreadAttributes(bionic_pthread_t thread, bionic_pthread_attr_t* attr, bool translated) {
  if (attr == nullptr) return kEINVAL;
  std::unique_lock<std::mutex> lock(g_threads_mutex);
  ReapCpuClocks();
  for (;;) {
    auto found = g_threads.find(thread);
    if (found == g_threads.end()) break;
    ThreadRecord* record = found->second;
    if (!record->attributes_ready) {
      g_thread_attributes_changed.wait(lock);
      continue;
    }
    if (translated && !record->translated_attributes_ready) return kEAGAIN;
    *attr = translated ? record->translated_attributes : record->attributes;
    if (record->detached) attr->flags |= kAttrDetached;
    return 0;
  }
  auto external = g_external_threads.find(thread);
  if (external == g_external_threads.end()) return kESRCH;
  if (external->second.handle == nullptr && thread != t_self) return kEAGAIN;
  if (translated && !external->second.translated_attributes_ready) return kEAGAIN;
  *attr = translated ? external->second.translated_attributes : external->second.attributes;
  return 0;
}

// --- mutex -----------------------------------------------------------------
// Word 0: bionic's type bits (14-15); word 1: the lock (0 free, 1 locked,
// 2 locked with waiters); word 2: the owner's tid; word 3: recursion count.

// Types: 0 normal, 1 recursive, 2 error-check (bionic's PTHREAD_MUTEX_*).
constexpr int kMutexNormal = 0, kMutexRecursive = 1;

int MutexType(bionic_pthread_mutex_t* m) { return (W(m, 0)->load() >> 14) & 3; }

void LockWord(Word* s) {
  int32_t c = 0;
  if (s->compare_exchange_strong(c, 1, std::memory_order_acquire)) return;
  if (c != 2) c = s->exchange(2, std::memory_order_acquire);
  while (c != 0) {
    FutexWait(s, 2);
    c = s->exchange(2, std::memory_order_acquire);
  }
}

// Android permits a null pthread deadline (an unbounded wait). Mutexes and
// rwlocks check a deadline only after their immediate acquisition fails;
// condition variables check it before releasing their mutex.
int CheckPthreadDeadline(const LinuxTimespec* abstime) {
  if (abstime == nullptr) return 0;
  if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) return kEINVAL;
  return abstime->tv_sec < 0 ? kETIMEDOUT : 0;
}

// A finite Windows wait is only a slice of an absolute Android deadline.
// Re-read the selected clock after a slice times out, including deadlines
// beyond DWORD milliseconds. Short realtime slices also observe wall-clock
// adjustments; a timeout from WaitOnAddress alone never ends an early wait.
bool WaitWordUntil(Word* s, int32_t expected, const LinuxTimespec* abstime, bool monotonic) {
  if (abstime == nullptr) return FutexWait(s, expected);
  for (;;) {
    uint32_t ms = MillisUntil(abstime, monotonic);
    if (ms == 0) return false;
    if (!monotonic && ms > 1000) ms = 1000;
    if (FutexWait(s, expected, ms)) return true;
  }
}

// Like LockWord, with Android's immediate-acquisition/validation order.
int LockWordTimed(Word* s, const LinuxTimespec* abstime, bool monotonic) {
  int32_t c = 0;
  if (s->compare_exchange_strong(c, 1, std::memory_order_acquire)) return 0;
  int result = CheckPthreadDeadline(abstime);
  if (result != 0) return result;
  if (c != 2) c = s->exchange(2, std::memory_order_acquire);
  while (c != 0) {
    if (!WaitWordUntil(s, 2, abstime, monotonic)) return kETIMEDOUT;
    c = s->exchange(2, std::memory_order_acquire);
  }
  return 0;
}

void UnlockWord(Word* s) {
  if (s->fetch_sub(1, std::memory_order_release) != 1) {
    s->store(0, std::memory_order_release);
    FutexWake(s, false);
  }
}

// The part of lock/trylock/timedlock before taking the word: recursion and
// error checks. Returns -1 to go on and take the word, else the result.
int MutexPreLock(bionic_pthread_mutex_t* m) {
  int type = MutexType(m);
  if (type == kMutexNormal) return -1;
  if (W(m, 2)->load(std::memory_order_relaxed) == SelfTid()) {
    if (type == kMutexRecursive) {
      W(m, 3)->fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    return kEDEADLK;
  }
  return -1;
}
void MutexTaken(bionic_pthread_mutex_t* m) {
  if (MutexType(m) == kMutexNormal) return;
  W(m, 2)->store(SelfTid(), std::memory_order_relaxed);
  W(m, 3)->store(1, std::memory_order_relaxed);
}

int MutexTimedLock(bionic_pthread_mutex_t* m, const LinuxTimespec* abstime, bool monotonic) {
  int result = MutexPreLock(m);
  if (result >= 0) return result;
  result = LockWordTimed(W(m, 1), abstime, monotonic);
  if (result != 0) return result;
  MutexTaken(m);
  return 0;
}

}  // namespace

// --- the System V entry points --------------------------------------------

extern "C" {

// Threads.

int BIONIC_SYSV gesher_bionic_pthread_attr_init(bionic_pthread_attr_t* attr) {
  *attr = DefaultThreadAttributes();
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_destroy(bionic_pthread_attr_t*) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_attr_setdetachstate(bionic_pthread_attr_t* attr, int s) {
  if (s == 1) {
    attr->flags |= kAttrDetached;
  } else if (s == 0) {
    attr->flags &= ~kAttrDetached;
  } else {
    return kEINVAL;
  }
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_getdetachstate(const bionic_pthread_attr_t* attr,
                                                         int* s) {
  *s = (attr->flags & kAttrDetached) ? 1 : 0;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_setstacksize(bionic_pthread_attr_t* attr,
                                                       uint64_t size) {
  if (size < 16384) return kEINVAL;
  attr->stack_size = size;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_getstacksize(const bionic_pthread_attr_t* attr,
                                                       uint64_t* size) {
  *size = attr->stack_size;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_setguardsize(bionic_pthread_attr_t* attr,
                                                       uint64_t size) {
  attr->guard_size = size;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_getguardsize(const bionic_pthread_attr_t* attr,
                                                       uint64_t* size) {
  *size = attr->guard_size;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_setschedpolicy(bionic_pthread_attr_t* attr, int p) {
  attr->sched_policy = p;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_setschedparam(bionic_pthread_attr_t* attr,
                                                        const int32_t* param) {
  attr->sched_priority = *param;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_getschedparam(const bionic_pthread_attr_t* attr,
                                                        int32_t* param) {
  *param = attr->sched_priority;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_setscope(bionic_pthread_attr_t*, int scope) {
  if (scope == 0) return 0;  // PTHREAD_SCOPE_SYSTEM
  return scope == 1 ? kEOPNOTSUPP : kEINVAL;  // PTHREAD_SCOPE_PROCESS
}
int BIONIC_SYSV gesher_bionic_pthread_attr_getstack(const bionic_pthread_attr_t* attr,
                                                   void** base, uint64_t* size) {
  *base = attr->stack_base;
  *size = attr->stack_size;
  return 0;
}

// Snapshots belong to retained pthread identities, not the querying thread or
// a reopened Windows TID. A newly created target may not have run yet; wait for
// its publication without retaining a record pointer across the unlocked wait.
int BIONIC_SYSV gesher_bionic_pthread_getattr_np(bionic_pthread_t thread,
                                                bionic_pthread_attr_t* attr) {
  return ThreadAttributes(thread, attr, false);
}

// A translated stack has a different allocation from its native callback
// stack. Publish values by retained pthread identity, never an Env pointer or
// a reopened Windows TID, so a pooled Env cannot revive a retired pthread ID.
int BIONIC_SYSV gesher_bionic___gesher_arm64_set_thread_stack(
    void* base, uint64_t size, uint64_t guard) {
  if (base == nullptr || size == 0 || guard >= size ||
      size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(base)) return kEINVAL;
  bionic_pthread_t id = SelfId();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  bionic_pthread_attr_t* attr = nullptr;
  bool* ready = nullptr;
  if (auto found = g_threads.find(id); found != g_threads.end()) {
    attr = &found->second->translated_attributes;
    ready = &found->second->translated_attributes_ready;
  } else if (auto external = g_external_threads.find(id); external != g_external_threads.end()) {
    attr = &external->second.translated_attributes;
    ready = &external->second.translated_attributes_ready;
  } else {
    return kESRCH;
  }
  *attr = DefaultThreadAttributes();
  attr->stack_base = base;
  attr->stack_size = size;
  attr->guard_size = guard;
  *ready = true;
  return 0;
}
int BIONIC_SYSV gesher_bionic___gesher_arm64_pthread_getattr_np(
    bionic_pthread_t thread, bionic_pthread_attr_t* attr) {
  return ThreadAttributes(thread, attr, true);
}

int BIONIC_SYSV gesher_bionic_pthread_create(bionic_pthread_t* thread,
                                            const bionic_pthread_attr_t* attr,
                                            void* (BIONIC_SYSV* start)(void*), void* arg) {
  auto* record = new ThreadRecord();
  record->detached = attr != nullptr && (attr->flags & kAttrDetached) != 0;
  uint64_t stack = attr != nullptr && attr->stack_size != 0 ? attr->stack_size : kDefaultStack;
  bionic_pthread_t id = g_next_id.fetch_add(1);
  auto* sa = new StartArg{start, arg, id, record};
  std::lock_guard<std::mutex> lock(g_threads_mutex);  // until the record is complete
  DWORD tid = 0;
  HANDLE h = CreateThread(nullptr, static_cast<SIZE_T>(stack), ThreadTrampoline, sa,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
  if (h == nullptr) {
    delete sa;
    delete record;
    return kEAGAIN;
  }
  record->handle = h;
  record->tid = static_cast<int32_t>(tid);
  g_threads[id] = record;
  if (thread) *thread = id;
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_join(bionic_pthread_t thread, void** retval) {
  ThreadRecord* record = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    auto it = g_threads.find(thread);
    if (it == g_threads.end() || it->second->detached) return kESRCH;
    record = it->second;
  }
  if (thread == t_self) return kEDEADLK;
  WaitForSingleObject(record->handle, INFINITE);
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  if (retval) *retval = record->retval;
  CloseHandle(record->handle);
  g_threads.erase(thread);
  delete record;
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_detach(bionic_pthread_t thread) {
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  auto it = g_threads.find(thread);
  if (it == g_threads.end()) return kESRCH;
  ThreadRecord* record = it->second;
  if (WaitForSingleObject(record->handle, 0) == WAIT_OBJECT_0) {
    // Already finished: nothing will free the record later.
    CloseHandle(record->handle);
    g_threads.erase(it);
    delete record;
  } else {
    record->detached = true;
  }
  return 0;
}

bionic_pthread_t BIONIC_SYSV gesher_bionic_pthread_self(void) { return SelfId(); }

int BIONIC_SYSV gesher_bionic_pthread_equal(bionic_pthread_t a, bionic_pthread_t b) {
  return a == b ? 1 : 0;
}

void BIONIC_SYSV gesher_bionic_pthread_exit(void* retval) {
  FinishThread(retval);
  ExitThread(0);
}

void BIONIC_SYSV gesher_bionic___pthread_cleanup_push(
    CleanupRecord* record, CleanupRoutine routine, void* argument) {
  t_cleanups.Push(record, routine, argument);
  ArmHostExit();
}

void BIONIC_SYSV gesher_bionic___pthread_cleanup_pop(CleanupRecord* record, int execute) {
  t_cleanups.Pop(record, execute != 0);
}

int32_t BIONIC_SYSV gesher_bionic_pthread_gettid_np(bionic_pthread_t thread) {
  if (thread == SelfId()) return SelfTid();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  auto it = g_threads.find(thread);
  return it == g_threads.end() ? -1 : it->second->tid;
}

// bionic's limits: 15 bytes and the NUL, ERANGE beyond. The name is the Windows
// thread description, which debuggers, /proc/self/task/<tid>/comm and
// pthread_getname_np/PR_GET_NAME (extra_sys_bionic.cc) read.
int BIONIC_SYSV gesher_bionic_pthread_setname_np(bionic_pthread_t thread, const char* name) {
  return PthreadSetName(thread, name);
}
int BIONIC_SYSV gesher_bionic_sched_yield(void) {
  SwitchToThread();
  return 0;
}

// Mutex attributes: bionic's pthread_mutexattr_t is a long; the type is in
// the low bits.

int BIONIC_SYSV gesher_bionic_pthread_mutexattr_init(bionic_pthread_mutexattr_t* a) {
  *a = 0;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_mutexattr_destroy(bionic_pthread_mutexattr_t*) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_mutexattr_settype(bionic_pthread_mutexattr_t* a, int t) {
  if (t < 0 || t > 2) return kEINVAL;
  *a = (*a & ~int64_t{0xf}) | t;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_mutexattr_gettype(const bionic_pthread_mutexattr_t* a,
                                                       int* t) {
  *t = static_cast<int>(*a & 0xf);
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_mutexattr_setpshared(bionic_pthread_mutexattr_t*, int) {
  return 0;
}

// Mutexes.

int BIONIC_SYSV gesher_bionic_pthread_mutex_init(bionic_pthread_mutex_t* m,
                                                const bionic_pthread_mutexattr_t* attr) {
  std::memset(m, 0, sizeof(*m));
  int type = attr != nullptr ? static_cast<int>(*attr & 3) : 0;
  W(m, 0)->store(type << 14);
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_mutex_destroy(bionic_pthread_mutex_t*) { return 0; }

int BIONIC_SYSV gesher_bionic_pthread_mutex_lock(bionic_pthread_mutex_t* m) {
  int r = MutexPreLock(m);
  if (r >= 0) return r;
  LockWord(W(m, 1));
  MutexTaken(m);
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_mutex_trylock(bionic_pthread_mutex_t* m) {
  int r = MutexPreLock(m);
  if (r == kEDEADLK) return kEBUSY;
  if (r >= 0) return r;
  int32_t c = 0;
  if (!W(m, 1)->compare_exchange_strong(c, 1, std::memory_order_acquire)) return kEBUSY;
  MutexTaken(m);
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_mutex_timedlock(bionic_pthread_mutex_t* m,
                                                     const LinuxTimespec* abstime) {
  return MutexTimedLock(m, abstime, false);
}
int BIONIC_SYSV gesher_bionic_pthread_mutex_timedlock_monotonic_np(
    bionic_pthread_mutex_t* m, const LinuxTimespec* abstime) {
  return MutexTimedLock(m, abstime, true);
}
int BIONIC_SYSV gesher_bionic_pthread_mutex_clocklock(
    bionic_pthread_mutex_t* m, int clock, const LinuxTimespec* abstime) {
  if (clock != 0 && clock != 1) return kEINVAL;
  return MutexTimedLock(m, abstime, clock == 1);
}

int BIONIC_SYSV gesher_bionic_pthread_mutex_unlock(bionic_pthread_mutex_t* m) {
  int type = MutexType(m);
  if (type != kMutexNormal) {
    if (W(m, 2)->load(std::memory_order_relaxed) != SelfTid()) return kEPERM;
    if (type == kMutexRecursive && W(m, 3)->fetch_sub(1, std::memory_order_relaxed) > 1) {
      return 0;
    }
    W(m, 2)->store(0, std::memory_order_relaxed);
    W(m, 3)->store(0, std::memory_order_relaxed);
  }
  UnlockWord(W(m, 1));
  return 0;
}

// Condition variables: word 0 is a sequence number bumped by every signal.
// Waiting on a recursive mutex held more than once is not supported (as in
// POSIX, it is undefined).

int BIONIC_SYSV gesher_bionic_pthread_condattr_init(bionic_pthread_condattr_t* a) {
  *a = 0;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_condattr_destroy(bionic_pthread_condattr_t*) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_condattr_setclock(bionic_pthread_condattr_t* a, int clock) {
  if (clock != 0 && clock != 1) return kEINVAL;
  *a = clock;
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_cond_init(bionic_pthread_cond_t* c,
                                               const bionic_pthread_condattr_t* attr) {
  std::memset(c, 0, sizeof(*c));
  // Word 1 remembers a CLOCK_MONOTONIC condattr (clock id 1) for timedwait.
  W(c, 1)->store(attr != nullptr && *attr == 1 ? 1 : 0);
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_cond_destroy(bionic_pthread_cond_t*) { return 0; }

namespace {
int CondWait(bionic_pthread_cond_t* c, bionic_pthread_mutex_t* m, const LinuxTimespec* abstime,
             bool monotonic) {
  int result = CheckPthreadDeadline(abstime);
  if (result != 0) return result;
  Word* seq = W(c, 0);
  int32_t observed = seq->load(std::memory_order_relaxed);
  gesher_bionic_pthread_mutex_unlock(m);
  bool timed_out = !WaitWordUntil(seq, observed, abstime, monotonic);
  gesher_bionic_pthread_mutex_lock(m);
  return timed_out ? kETIMEDOUT : 0;
}
}  // namespace

int BIONIC_SYSV gesher_bionic_pthread_cond_wait(bionic_pthread_cond_t* c,
                                               bionic_pthread_mutex_t* m) {
  return CondWait(c, m, nullptr, false);
}
int BIONIC_SYSV gesher_bionic_pthread_cond_timedwait(bionic_pthread_cond_t* c,
                                                    bionic_pthread_mutex_t* m,
                                                    const LinuxTimespec* abstime) {
  return CondWait(c, m, abstime, W(c, 1)->load() == 1);
}
int BIONIC_SYSV gesher_bionic_pthread_cond_timedwait_monotonic_np(bionic_pthread_cond_t* c,
                                                                 bionic_pthread_mutex_t* m,
                                                                 const LinuxTimespec* abstime) {
  return CondWait(c, m, abstime, true);
}
int BIONIC_SYSV gesher_bionic_pthread_cond_clockwait(
    bionic_pthread_cond_t* c, bionic_pthread_mutex_t* m, int clock,
    const LinuxTimespec* abstime) {
  if (clock != 0 && clock != 1) return kEINVAL;
  // An explicit clock overrides the condvar attribute without mutating it.
  return CondWait(c, m, abstime, clock == 1);
}
int BIONIC_SYSV gesher_bionic_pthread_cond_signal(bionic_pthread_cond_t* c) {
  W(c, 0)->fetch_add(1, std::memory_order_release);
  FutexWake(W(c, 0), false);
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_cond_broadcast(bionic_pthread_cond_t* c) {
  W(c, 0)->fetch_add(1, std::memory_order_release);
  FutexWake(W(c, 0), true);
  return 0;
}

// Read-write locks: word 0 is -1 (a writer), 0 (free) or the reader count;
// word 1 records the writer's thread id. Both fit the existing LP64 object.
// Android reserves 29 bits for readers; reject exhaustion before addition.
constexpr int32_t kRwlockMaxReaders = (1 << 29) - 1;

int BIONIC_SYSV gesher_bionic_pthread_rwlock_init(bionic_pthread_rwlock_t* rw,
                                                 const bionic_pthread_rwlockattr_t*) {
  std::memset(rw, 0, sizeof(*rw));
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_destroy(bionic_pthread_rwlock_t* rw) {
  return W(rw, 0)->load(std::memory_order_relaxed) == 0 ? 0 : kEBUSY;
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_tryrdlock(bionic_pthread_rwlock_t* rw) {
  Word* s = W(rw, 0);
  int32_t v = s->load(std::memory_order_relaxed);
  while (v >= 0) {
    if (v >= kRwlockMaxReaders) return kEAGAIN;
    if (s->compare_exchange_weak(v, v + 1, std::memory_order_acquire)) return 0;
  }
  return kEBUSY;
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_trywrlock(bionic_pthread_rwlock_t* rw) {
  int32_t v = 0;
  if (!W(rw, 0)->compare_exchange_strong(v, -1, std::memory_order_acquire)) return kEBUSY;
  W(rw, 1)->store(SelfTid(), std::memory_order_relaxed);
  return 0;
}

namespace {
int RwlockTimedLock(bionic_pthread_rwlock_t* rw, const LinuxTimespec* abstime,
                    bool monotonic, bool writer) {
  if (W(rw, 1)->load(std::memory_order_relaxed) == SelfTid()) return kEDEADLK;
  Word* s = W(rw, 0);
  for (;;) {
    int result = writer ? gesher_bionic_pthread_rwlock_trywrlock(rw)
                        : gesher_bionic_pthread_rwlock_tryrdlock(rw);
    if (result != kEBUSY) return result;
    result = CheckPthreadDeadline(abstime);
    if (result != 0) return result;
    int32_t observed = s->load(std::memory_order_relaxed);
    // An unlock can have raced the try call. Do not wait on a free/readable
    // state and lose the wake that happened before this snapshot.
    if (writer ? observed == 0 : observed >= 0) continue;
    if (!WaitWordUntil(s, observed, abstime, monotonic)) return kETIMEDOUT;
  }
}
}  // namespace

int BIONIC_SYSV gesher_bionic_pthread_rwlock_rdlock(bionic_pthread_rwlock_t* rw) {
  return RwlockTimedLock(rw, nullptr, true, false);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_wrlock(bionic_pthread_rwlock_t* rw) {
  return RwlockTimedLock(rw, nullptr, true, true);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_timedrdlock(
    bionic_pthread_rwlock_t* rw, const LinuxTimespec* abstime) {
  return RwlockTimedLock(rw, abstime, false, false);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_timedwrlock(
    bionic_pthread_rwlock_t* rw, const LinuxTimespec* abstime) {
  return RwlockTimedLock(rw, abstime, false, true);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_timedrdlock_monotonic_np(
    bionic_pthread_rwlock_t* rw, const LinuxTimespec* abstime) {
  return RwlockTimedLock(rw, abstime, true, false);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_timedwrlock_monotonic_np(
    bionic_pthread_rwlock_t* rw, const LinuxTimespec* abstime) {
  return RwlockTimedLock(rw, abstime, true, true);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_clockrdlock(
    bionic_pthread_rwlock_t* rw, int clock, const LinuxTimespec* abstime) {
  if (clock != 0 && clock != 1) return kEINVAL;
  return RwlockTimedLock(rw, abstime, clock == 1, false);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_clockwrlock(
    bionic_pthread_rwlock_t* rw, int clock, const LinuxTimespec* abstime) {
  if (clock != 0 && clock != 1) return kEINVAL;
  return RwlockTimedLock(rw, abstime, clock == 1, true);
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_unlock(bionic_pthread_rwlock_t* rw) {
  Word* s = W(rw, 0);
  int32_t v = s->load(std::memory_order_relaxed);
  if (v == -1) {
    if (W(rw, 1)->load(std::memory_order_relaxed) != SelfTid()) return kEPERM;
    W(rw, 1)->store(0, std::memory_order_relaxed);
    s->store(0, std::memory_order_release);
    FutexWake(s, true);
  } else if (v > 0) {
    if (s->fetch_sub(1, std::memory_order_release) == 1) FutexWake(s, true);
  } else {
    return kEPERM;
  }
  return 0;
}

// Once.

int BIONIC_SYSV gesher_bionic_pthread_once(bionic_pthread_once_t* once,
                                          void (BIONIC_SYSV* init)(void)) {
  Word* s = reinterpret_cast<Word*>(once);
  if (s->load(std::memory_order_acquire) == 2) return 0;
  int32_t expected = 0;
  if (s->compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
    init();
    s->store(2, std::memory_order_release);
    FutexWake(s, true);
    return 0;
  }
  for (int32_t v; (v = s->load(std::memory_order_acquire)) != 2;) FutexWait(s, v);
  return 0;
}

// Thread-specific keys.

int BIONIC_SYSV gesher_bionic_pthread_key_create(bionic_pthread_key_t* key,
                                                void (BIONIC_SYSV* dtor)(void*)) {
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  for (int i = 0; i < kKeysMax; ++i) {
    if (!g_key_used[i] && g_key_generations[i] != UINT64_MAX) {
      ++g_key_generations[i];
      g_key_used[i] = true;
      g_key_dtors[i] = dtor;
      *key = i;
      return 0;
    }
  }
  return kEAGAIN;
}

int BIONIC_SYSV gesher_bionic_pthread_key_delete(bionic_pthread_key_t key) {
  if (key < 0 || key >= kKeysMax) return kEINVAL;
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  if (!g_key_used[key]) return kEINVAL;
  g_key_used[key] = false;
  g_key_dtors[key] = nullptr;
  return 0;
}

void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t key) {
  if (key < 0 || key >= kKeysMax) return nullptr;
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  if (!g_key_used[key] || t_key_generations[key] != g_key_generations[key]) {
    t_key_values[key] = nullptr;
    t_key_generations[key] = 0;
    return nullptr;
  }
  return t_key_values[key];
}

int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t key, const void* value) {
  if (key < 0 || key >= kKeysMax) return kEINVAL;
  {
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    if (!g_key_used[key]) return kEINVAL;
    t_key_values[key] = const_cast<void*>(value);
    t_key_generations[key] = g_key_generations[key];
  }
  if (value != nullptr) ArmHostExit();
  return 0;
}

// C++ thread_local destructors (libc++abi calls this for every thread_local
// object with a destructor). Run when the thread ends, before its keys.
int BIONIC_SYSV gesher_bionic_cxa_thread_atexit_impl(void (BIONIC_SYSV* dtor)(void*), void* obj,
                                                    void* /*dso*/) {
  if (t_thread_atexit == nullptr) t_thread_atexit = new std::vector<ThreadAtExit>();
  t_thread_atexit->push_back({dtor, obj});
  ArmHostExit();
  return 0;
}

int BIONIC_SYSV gesher_bionic_register_atfork(void*, void*, void*, void*) { return 0; }

}  // extern "C"

uint64_t PthreadSelfId() { return SelfId(); }

namespace {
// g_threads_mutex protects the retained objects through Get/SetThreadDescription.
// No thread ID is reopened and no borrowed handle escapes this critical section.
HANDLE NameTarget(uint64_t thread, uint64_t self) {
  HANDLE target = nullptr;
  if (thread == self) target = GetCurrentThread();
  else {
    auto found = g_threads.find(thread);
    if (found != g_threads.end()) target = found->second->handle;
    auto external = g_external_threads.find(thread);
    if (external != g_external_threads.end()) target = external->second.handle;
  }
  if (!target || GetProcessIdOfThread(target) != GetCurrentProcessId() ||
      WaitForSingleObject(target, 0) != WAIT_TIMEOUT) return nullptr;
  return target;
}
}
int PthreadSignalTarget(uint64_t thread, void** handle) {
  uint64_t self = SelfId();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  ReapCpuClocks();
  HANDLE target = NameTarget(thread, self);
  if (target == nullptr) return kESRCH;
  HANDLE retained = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(), &retained,
                       THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
    return kEAGAIN;
  }
  *handle = retained;
  return 0;
}
int PthreadSetName(uint64_t thread, const char* name) {
  if (name == nullptr) return kEINVAL;
  if (std::strlen(name) >= 16) return kERANGE;
  wchar_t wide[16];
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 16) <= 0) return kEINVAL;
  uint64_t self = SelfId();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  ReapCpuClocks();
  HANDLE target = NameTarget(thread, self);
  if (!target) return kESRCH;
  return SUCCEEDED(SetThreadDescription(target, wide)) ? 0 : kESRCH;
}
int PthreadGetName(uint64_t thread, char* name, size_t size) {
  if (name == nullptr) return kEINVAL;
  if (size < 16) return kERANGE;
  uint64_t self = SelfId();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  ReapCpuClocks();
  HANDLE target = NameTarget(thread, self);
  if (!target) return kESRCH;
  PWSTR description = nullptr;
  if (FAILED(GetThreadDescription(target, &description))) return kESRCH;
  int count = description ? WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr) : 1;
  std::string text(count > 0 ? count : 0, '\0');
  bool converted = count > 0 && (!description ||
      WideCharToMultiByte(CP_UTF8, 0, description, -1, text.data(), count, nullptr, nullptr) == count);
  if (description) LocalFree(description);
  if (!converted) return kEINVAL;
  text.resize(static_cast<size_t>(count - 1));
  size_t length = text.size() < 15 ? text.size() : 15;
  if (length < text.size()) {
    while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
  }
  std::memcpy(name, text.data(), length);
  name[length] = 0;
  return 0;
}

int PthreadCpuClock(uint64_t thread, int* clock_id) {
  if (clock_id == nullptr) return kEINVAL;
  // SelfId can allocate an identity for a host/ART-created caller. Other
  // identities must come from the live bionic registry, never a cast handle.
  uint64_t self = SelfId();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  ReapCpuClocks();
  HANDLE target = nullptr;
  if (thread == self) {
    target = GetCurrentThread();
  } else {
    auto found = g_threads.find(thread);
    if (found != g_threads.end()) target = found->second->handle;
    auto external = g_external_threads.find(thread);
    if (external != g_external_threads.end()) {
      target = external->second.handle;
      if (target == nullptr) return kEAGAIN;
    }
  }
  // A previously acquired clock also retains an externally created caller.
  for (const auto& clock : g_cpu_clocks) {
    if (clock.handle != nullptr && clock.thread == thread) {
      *clock_id = clock.id;
      return 0;
    }
  }
  if (target == nullptr || WaitForSingleObject(target, 0) != WAIT_TIMEOUT) return kESRCH;
  if (g_next_cpu_clock < INT_MIN) return kEAGAIN;
  for (auto& clock : g_cpu_clocks) {
    if (clock.handle != nullptr) continue;
    HANDLE retained = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(), &retained,
                         THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) return kEAGAIN;
    clock = {thread, retained, static_cast<int>(g_next_cpu_clock--)};
    *clock_id = clock.id;
    return 0;
  }
  return kEAGAIN;
}

int ReadPthreadCpuClock(int clock_id, int64_t* nanos) {
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  ReapCpuClocks();
  for (const auto& clock : g_cpu_clocks) {
    if (clock.handle == nullptr || clock.id != clock_id) continue;
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(clock.handle, &created, &exited, &kernel, &user)) return kEINVAL;
    auto ticks = [](FILETIME t) {
      return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    *nanos = static_cast<int64_t>((ticks(kernel) + ticks(user)) * 100);
    return 0;
  }
  return kEINVAL;
}

void RegisterPthread(SymbolTable& t) {
#define P(name) t[#name] = reinterpret_cast<void*>(&gesher_bionic_##name)
  P(pthread_attr_init);
  P(pthread_attr_destroy);
  P(pthread_attr_setdetachstate);
  P(pthread_attr_getdetachstate);
  P(pthread_attr_setstacksize);
  P(pthread_attr_getstacksize);
  P(pthread_attr_setguardsize);
  P(pthread_attr_getguardsize);
  P(pthread_attr_setschedpolicy);
  P(pthread_attr_setschedparam);
  P(pthread_attr_getschedparam);
  P(pthread_attr_setscope);
  P(pthread_attr_getstack);
  P(pthread_getattr_np);
  P(__gesher_arm64_set_thread_stack);
  P(__gesher_arm64_pthread_getattr_np);
  P(pthread_create);
  P(pthread_join);
  P(pthread_detach);
  P(pthread_self);
  P(pthread_equal);
  P(pthread_exit);
  P(__pthread_cleanup_push);
  P(__pthread_cleanup_pop);
  P(pthread_gettid_np);
  P(pthread_setname_np);
  P(sched_yield);
  P(pthread_mutexattr_init);
  P(pthread_mutexattr_destroy);
  P(pthread_mutexattr_settype);
  P(pthread_mutexattr_gettype);
  P(pthread_mutexattr_setpshared);
  P(pthread_mutex_init);
  P(pthread_mutex_destroy);
  P(pthread_mutex_lock);
  P(pthread_mutex_trylock);
  P(pthread_mutex_timedlock);
  P(pthread_mutex_timedlock_monotonic_np);
  P(pthread_mutex_clocklock);
  P(pthread_mutex_unlock);
  P(pthread_condattr_init);
  P(pthread_condattr_destroy);
  P(pthread_condattr_setclock);
  P(pthread_cond_init);
  P(pthread_cond_destroy);
  P(pthread_cond_wait);
  P(pthread_cond_timedwait);
  P(pthread_cond_timedwait_monotonic_np);
  P(pthread_cond_clockwait);
  P(pthread_cond_signal);
  P(pthread_cond_broadcast);
  P(pthread_rwlock_init);
  P(pthread_rwlock_destroy);
  P(pthread_rwlock_rdlock);
  P(pthread_rwlock_tryrdlock);
  P(pthread_rwlock_wrlock);
  P(pthread_rwlock_trywrlock);
  P(pthread_rwlock_timedrdlock);
  P(pthread_rwlock_timedwrlock);
  P(pthread_rwlock_timedrdlock_monotonic_np);
  P(pthread_rwlock_timedwrlock_monotonic_np);
  P(pthread_rwlock_clockrdlock);
  P(pthread_rwlock_clockwrlock);
  P(pthread_rwlock_unlock);
  P(pthread_once);
  P(pthread_key_create);
  P(pthread_key_delete);
  P(pthread_getspecific);
  P(pthread_setspecific);
#undef P
  t["__cxa_thread_atexit_impl"] = reinterpret_cast<void*>(&gesher_bionic_cxa_thread_atexit_impl);
  t["__register_atfork"] = reinterpret_cast<void*>(&gesher_bionic_register_atfork);
  t["pthread_atfork"] = reinterpret_cast<void*>(&gesher_bionic_register_atfork);
}

}  // namespace gesher::bionic
