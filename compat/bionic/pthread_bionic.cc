// gesher bionic shim: bionic's pthread ABI over Windows.
//
// The app's .so uses bionic's pthread types (abi_types.h) and ids, whose
// layouts differ from winpthreads', so these cannot forward to winpthreads;
// they are implemented directly on Windows primitives (threads, WaitOnAddress
// futexes). Each pthread_create'd thread sets up %fs for loaded code before it
// runs (docs/decisions/0014-native-libs-fs-base.md).
//
// All entry points are System V (BIONIC_SYSV), as the .so calls them.

#include "abi_types.h"

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

// --- futex helpers on the mutex/cond state words --------------------------

void FutexWait(volatile int32_t* addr, int32_t expected) {
  int32_t cmp = expected;
  WaitOnAddress(const_cast<int32_t*>(addr), &cmp, sizeof(int32_t), INFINITE);
}
void FutexWake(volatile int32_t* addr, bool all) {
  if (all) {
    WakeByAddressAll(const_cast<int32_t*>(addr));
  } else {
    WakeByAddressSingle(const_cast<int32_t*>(addr));
  }
}

std::atomic<int32_t>* State(bionic_pthread_mutex_t* m) {
  return reinterpret_cast<std::atomic<int32_t>*>(&m->__private[0]);
}
std::atomic<int32_t>* State(bionic_pthread_cond_t* c) {
  return reinterpret_cast<std::atomic<int32_t>*>(&c->__private[0]);
}

// --- threads ---------------------------------------------------------------

struct ThreadRecord {
  HANDLE handle = nullptr;
  void* retval = nullptr;
  std::atomic<int32_t> done{0};
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

DWORD WINAPI ThreadTrampoline(void* raw) {
  StartArg* sa = static_cast<StartArg*>(raw);
  t_self = sa->id;
  t_record = sa->record;
  gesher::elf::SetupThreadTls();  // %fs for this thread's loaded code
  void* ret = sa->start(sa->arg);
  sa->record->retval = ret;
  sa->record->done.store(1, std::memory_order_release);
  FutexWake(reinterpret_cast<volatile int32_t*>(&sa->record->done), true);
  delete sa;
  return 0;
}

bionic_pthread_t SelfId() {
  if (t_self == 0) {
    t_self = g_next_id.fetch_add(1);
  }
  return t_self;
}

}  // namespace

extern "C" {

int BIONIC_SYSV gesher_bionic_pthread_create(bionic_pthread_t* thread,
                                            const bionic_pthread_attr_t* /*attr*/,
                                            void* (BIONIC_SYSV* start)(void*), void* arg) {
  auto* record = new ThreadRecord();
  bionic_pthread_t id = g_next_id.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads[id] = record;
  }
  auto* sa = new StartArg{start, arg, id, record};
  HANDLE h = CreateThread(nullptr, 0, ThreadTrampoline, sa, 0, nullptr);
  if (h == nullptr) {
    delete sa;
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads.erase(id);
    delete record;
    return 11;  // EAGAIN
  }
  record->handle = h;
  if (thread) *thread = id;
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_join(bionic_pthread_t thread, void** retval) {
  ThreadRecord* record = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    auto it = g_threads.find(thread);
    if (it == g_threads.end()) return 3;  // ESRCH
    record = it->second;
  }
  WaitForSingleObject(record->handle, INFINITE);
  if (retval) *retval = record->retval;
  CloseHandle(record->handle);
  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads.erase(thread);
  }
  delete record;
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_detach(bionic_pthread_t thread) {
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  auto it = g_threads.find(thread);
  if (it == g_threads.end()) return 3;
  it->second->detached = true;
  CloseHandle(it->second->handle);
  delete it->second;
  g_threads.erase(it);
  return 0;
}

bionic_pthread_t BIONIC_SYSV gesher_bionic_pthread_self(void) { return SelfId(); }

int BIONIC_SYSV gesher_bionic_pthread_equal(bionic_pthread_t a, bionic_pthread_t b) {
  return a == b ? 1 : 0;
}

void BIONIC_SYSV gesher_bionic_pthread_exit(void* retval) {
  if (t_record) {
    t_record->retval = retval;
    t_record->done.store(1, std::memory_order_release);
    FutexWake(reinterpret_cast<volatile int32_t*>(&t_record->done), true);
  }
  ExitThread(0);
}

// --- mutex (three-state futex: 0 free, 1 locked, 2 locked+waiters) --------

int BIONIC_SYSV gesher_bionic_pthread_mutex_init(bionic_pthread_mutex_t* m,
                                                const bionic_pthread_mutexattr_t* /*attr*/) {
  std::memset(m, 0, sizeof(*m));
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_mutex_destroy(bionic_pthread_mutex_t* /*m*/) { return 0; }

int BIONIC_SYSV gesher_bionic_pthread_mutex_lock(bionic_pthread_mutex_t* m) {
  std::atomic<int32_t>* s = State(m);
  int32_t c = 0;
  if (s->compare_exchange_strong(c, 1, std::memory_order_acquire)) return 0;
  if (c != 2) c = s->exchange(2, std::memory_order_acquire);
  while (c != 0) {
    FutexWait(reinterpret_cast<volatile int32_t*>(s), 2);
    c = s->exchange(2, std::memory_order_acquire);
  }
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_mutex_trylock(bionic_pthread_mutex_t* m) {
  std::atomic<int32_t>* s = State(m);
  int32_t c = 0;
  return s->compare_exchange_strong(c, 1, std::memory_order_acquire) ? 0 : 16;  // EBUSY
}

int BIONIC_SYSV gesher_bionic_pthread_mutex_unlock(bionic_pthread_mutex_t* m) {
  std::atomic<int32_t>* s = State(m);
  if (s->fetch_sub(1, std::memory_order_release) != 1) {
    s->store(0, std::memory_order_release);
    FutexWake(reinterpret_cast<volatile int32_t*>(s), false);
  }
  return 0;
}

// --- condition variable ----------------------------------------------------

int BIONIC_SYSV gesher_bionic_pthread_cond_init(bionic_pthread_cond_t* c,
                                               const bionic_pthread_condattr_t* /*attr*/) {
  std::memset(c, 0, sizeof(*c));
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_cond_destroy(bionic_pthread_cond_t* /*c*/) { return 0; }

int BIONIC_SYSV gesher_bionic_pthread_cond_wait(bionic_pthread_cond_t* c,
                                               bionic_pthread_mutex_t* m) {
  std::atomic<int32_t>* seq = State(c);
  int32_t observed = seq->load(std::memory_order_relaxed);
  gesher_bionic_pthread_mutex_unlock(m);
  FutexWait(reinterpret_cast<volatile int32_t*>(seq), observed);
  gesher_bionic_pthread_mutex_lock(m);
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_cond_signal(bionic_pthread_cond_t* c) {
  State(c)->fetch_add(1, std::memory_order_release);
  FutexWake(reinterpret_cast<volatile int32_t*>(State(c)), false);
  return 0;
}
int BIONIC_SYSV gesher_bionic_pthread_cond_broadcast(bionic_pthread_cond_t* c) {
  State(c)->fetch_add(1, std::memory_order_release);
  FutexWake(reinterpret_cast<volatile int32_t*>(State(c)), true);
  return 0;
}

// --- once ------------------------------------------------------------------

int BIONIC_SYSV gesher_bionic_pthread_once(bionic_pthread_once_t* once,
                                          void (BIONIC_SYSV* init)(void)) {
  auto* s = reinterpret_cast<std::atomic<int32_t>*>(once);
  int32_t state = s->load(std::memory_order_acquire);
  if (state == 2) return 0;
  int32_t expected = 0;
  if (s->compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
    init();
    s->store(2, std::memory_order_release);
    FutexWake(reinterpret_cast<volatile int32_t*>(s), true);
    return 0;
  }
  while (s->load(std::memory_order_acquire) != 2) {
    FutexWait(reinterpret_cast<volatile int32_t*>(s), 1);
  }
  return 0;
}

// --- thread-specific keys --------------------------------------------------

namespace {
std::mutex g_keys_mutex;
std::vector<void (BIONIC_SYSV*)(void*)> g_key_dtors;  // index = key
std::vector<bool> g_key_used;
thread_local std::vector<void*> t_key_values;
}  // namespace

int BIONIC_SYSV gesher_bionic_pthread_key_create(bionic_pthread_key_t* key,
                                                void (BIONIC_SYSV* dtor)(void*)) {
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  for (size_t i = 0; i < g_key_used.size(); ++i) {
    if (!g_key_used[i]) {
      g_key_used[i] = true;
      g_key_dtors[i] = dtor;
      *key = static_cast<bionic_pthread_key_t>(i);
      return 0;
    }
  }
  g_key_used.push_back(true);
  g_key_dtors.push_back(dtor);
  *key = static_cast<bionic_pthread_key_t>(g_key_used.size() - 1);
  return 0;
}

int BIONIC_SYSV gesher_bionic_pthread_key_delete(bionic_pthread_key_t key) {
  std::lock_guard<std::mutex> lock(g_keys_mutex);
  if (key >= 0 && static_cast<size_t>(key) < g_key_used.size()) g_key_used[key] = false;
  return 0;
}

void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t key) {
  if (key < 0 || static_cast<size_t>(key) >= t_key_values.size()) return nullptr;
  return t_key_values[key];
}

int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t key, const void* value) {
  if (key < 0) return 22;  // EINVAL
  if (static_cast<size_t>(key) >= t_key_values.size()) {
    t_key_values.resize(key + 1, nullptr);
  }
  t_key_values[key] = const_cast<void*>(value);
  return 0;
}

}  // extern "C"

}  // namespace gesher::bionic
