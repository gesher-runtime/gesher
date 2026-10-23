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

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
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
thread_local void* t_key_values[kKeysMax] = {};

struct ThreadAtExit {
  void (BIONIC_SYSV* dtor)(void*);
  void* obj;
};
thread_local std::vector<ThreadAtExit>* t_thread_atexit = nullptr;

void RunThreadExit() {
  // C++ thread_local destructors first, last registered first (as bionic).
  if (t_thread_atexit != nullptr) {
    while (!t_thread_atexit->empty()) {
      ThreadAtExit e = t_thread_atexit->back();
      t_thread_atexit->pop_back();
      e.dtor(e.obj);
    }
    delete t_thread_atexit;
    t_thread_atexit = nullptr;
  }
  for (int pass = 0; pass < kDestructorPasses; ++pass) {
    bool any = false;
    for (int k = 0; k < kKeysMax; ++k) {
      void* value = t_key_values[k];
      if (value == nullptr) continue;
      void (BIONIC_SYSV* dtor)(void*) = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_keys_mutex);
        if (g_key_used[k]) dtor = g_key_dtors[k];
      }
      t_key_values[k] = nullptr;
      if (dtor != nullptr) {
        dtor(value);
        any = true;
      }
    }
    if (!any) break;
  }
}

// --- threads ---------------------------------------------------------------

constexpr uint32_t kAttrDetached = 1;  // bionic PTHREAD_ATTR_FLAG_DETACHED
constexpr uint64_t kDefaultStack = 1024 * 1024;

struct ThreadRecord {
  HANDLE handle = nullptr;
  void* retval = nullptr;
  int32_t tid = 0;
  bool detached = false;
};

std::mutex g_threads_mutex;
std::unordered_map<bionic_pthread_t, ThreadRecord*> g_threads;
std::atomic<bionic_pthread_t> g_next_id{1};

thread_local bionic_pthread_t t_self = 0;
thread_local ThreadRecord* t_record = nullptr;

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
  gesher::elf::SetupThreadTls();  // the thread's TLS block for loaded code
  void* ret = sa.start(sa.arg);
  FinishThread(ret);
  return 0;
}

bionic_pthread_t SelfId() {
  if (t_self == 0) t_self = g_next_id.fetch_add(1);
  return t_self;
}

// --- mutex -----------------------------------------------------------------
// Word 0: bionic's type bits (14-15); word 1: the lock (0 free, 1 locked,
// 2 locked with waiters); word 2: the owner's tid; word 3: recursion count.

constexpr int kMutexNormal = 0, kMutexRecursive = 1, kMutexErrorCheck = 2;

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

// Like LockWord, with a deadline; false on timeout.
bool LockWordTimed(Word* s, const LinuxTimespec* abstime, bool monotonic) {
  int32_t c = 0;
  if (s->compare_exchange_strong(c, 1, std::memory_order_acquire)) return true;
  if (c != 2) c = s->exchange(2, std::memory_order_acquire);
  while (c != 0) {
    uint32_t ms = MillisUntil(abstime, monotonic);
    if (ms == 0) return false;
    FutexWait(s, 2, ms);
    c = s->exchange(2, std::memory_order_acquire);
  }
  return true;
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

}  // namespace

// --- the System V entry points --------------------------------------------

extern "C" {

// Threads.

int BIONIC_SYSV gesher_bionic_pthread_attr_init(bionic_pthread_attr_t* attr) {
  std::memset(attr, 0, sizeof(*attr));
  attr->stack_size = kDefaultStack;
  attr->guard_size = 4096;
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_attr_destroy(bionic_pthread_attr_t*) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_attr_setdetachstate(bionic_pthread_attr_t* attr, int s) {
  if (s) {
    attr->flags |= kAttrDetached;
  } else {
    attr->flags &= ~kAttrDetached;
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
int BIONIC_SYSV gesher_bionic_pthread_attr_setscope(bionic_pthread_attr_t*, int) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_attr_getstack(const bionic_pthread_attr_t* attr,
                                                   void** base, uint64_t* size) {
  *base = attr->stack_base;
  *size = attr->stack_size;
  return 0;
}

// The calling thread's stack, for pthread_getattr_np(pthread_self()).
int BIONIC_SYSV gesher_bionic_pthread_getattr_np(bionic_pthread_t, bionic_pthread_attr_t* attr) {
  gesher_bionic_pthread_attr_init(attr);
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  attr->stack_base = reinterpret_cast<void*>(low);
  attr->stack_size = high - low;
  return 0;
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

int32_t BIONIC_SYSV gesher_bionic_pthread_gettid_np(bionic_pthread_t thread) {
  if (thread == SelfId()) return SelfTid();
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  auto it = g_threads.find(thread);
  return it == g_threads.end() ? -1 : it->second->tid;
}

int BIONIC_SYSV gesher_bionic_pthread_setname_np(bionic_pthread_t, const char*) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_kill(bionic_pthread_t, int sig) {
  return sig == 0 ? 0 : kENOSYS;
}
int BIONIC_SYSV gesher_bionic_pthread_sigmask(int, const void*, void*) { return 0; }
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
  int r = MutexPreLock(m);
  if (r >= 0) return r;
  if (!LockWordTimed(W(m, 1), abstime, false)) return kETIMEDOUT;
  MutexTaken(m);
  return 0;
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
  Word* seq = W(c, 0);
  int32_t observed = seq->load(std::memory_order_relaxed);
  gesher_bionic_pthread_mutex_unlock(m);
  bool timed_out = false;
  if (abstime == nullptr) {
    FutexWait(seq, observed);
  } else {
    uint32_t ms = MillisUntil(abstime, monotonic);
    timed_out = ms == 0 || !FutexWait(seq, observed, ms);
  }
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

// Read-write locks: word 0 is -1 (a writer), 0 (free) or the reader count.

int BIONIC_SYSV gesher_bionic_pthread_rwlock_init(bionic_pthread_rwlock_t* rw,
                                                 const bionic_pthread_rwlockattr_t*) {
  std::memset(rw, 0, sizeof(*rw));
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_destroy(bionic_pthread_rwlock_t*) { return 0; }
int BIONIC_SYSV gesher_bionic_pthread_rwlock_tryrdlock(bionic_pthread_rwlock_t* rw) {
  Word* s = W(rw, 0);
  int32_t v = s->load(std::memory_order_relaxed);
  while (v >= 0) {
    if (s->compare_exchange_weak(v, v + 1, std::memory_order_acquire)) return 0;
  }
  return kEBUSY;
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_rdlock(bionic_pthread_rwlock_t* rw) {
  Word* s = W(rw, 0);
  for (;;) {
    int32_t v = s->load(std::memory_order_relaxed);
    if (v >= 0) {
      if (s->compare_exchange_weak(v, v + 1, std::memory_order_acquire)) return 0;
      continue;
    }
    FutexWait(s, v);
  }
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_trywrlock(bionic_pthread_rwlock_t* rw) {
  int32_t v = 0;
  return W(rw, 0)->compare_exchange_strong(v, -1, std::memory_order_acquire) ? 0 : kEBUSY;
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_wrlock(bionic_pthread_rwlock_t* rw) {
  Word* s = W(rw, 0);
  for (;;) {
    int32_t v = 0;
    if (s->compare_exchange_weak(v, -1, std::memory_order_acquire)) return 0;
    if (v != 0) FutexWait(s, v);
  }
}
int BIONIC_SYSV gesher_bionic_pthread_rwlock_unlock(bionic_pthread_rwlock_t* rw) {
  Word* s = W(rw, 0);
  int32_t v = s->load(std::memory_order_relaxed);
  if (v == -1) {
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
    if (!g_key_used[i]) {
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
  g_key_used[key] = false;
  g_key_dtors[key] = nullptr;
  return 0;
}

void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t key) {
  if (key < 0 || key >= kKeysMax) return nullptr;
  return t_key_values[key];
}

int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t key, const void* value) {
  if (key < 0 || key >= kKeysMax) return kEINVAL;
  t_key_values[key] = const_cast<void*>(value);
  return 0;
}

// C++ thread_local destructors (libc++abi calls this for every thread_local
// object with a destructor). Run when a shim-created thread ends.
int BIONIC_SYSV gesher_bionic_cxa_thread_atexit_impl(void (BIONIC_SYSV* dtor)(void*), void* obj,
                                                    void* /*dso*/) {
  if (t_thread_atexit == nullptr) t_thread_atexit = new std::vector<ThreadAtExit>();
  t_thread_atexit->push_back({dtor, obj});
  return 0;
}

int BIONIC_SYSV gesher_bionic_register_atfork(void*, void*, void*, void*) { return 0; }

}  // extern "C"

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
  P(pthread_create);
  P(pthread_join);
  P(pthread_detach);
  P(pthread_self);
  P(pthread_equal);
  P(pthread_exit);
  P(pthread_gettid_np);
  P(pthread_setname_np);
  P(pthread_kill);
  P(pthread_sigmask);
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
  P(pthread_mutex_unlock);
  P(pthread_condattr_init);
  P(pthread_condattr_destroy);
  P(pthread_condattr_setclock);
  P(pthread_cond_init);
  P(pthread_cond_destroy);
  P(pthread_cond_wait);
  P(pthread_cond_timedwait);
  P(pthread_cond_timedwait_monotonic_np);
  P(pthread_cond_signal);
  P(pthread_cond_broadcast);
  P(pthread_rwlock_init);
  P(pthread_rwlock_destroy);
  P(pthread_rwlock_rdlock);
  P(pthread_rwlock_tryrdlock);
  P(pthread_rwlock_wrlock);
  P(pthread_rwlock_trywrlock);
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
