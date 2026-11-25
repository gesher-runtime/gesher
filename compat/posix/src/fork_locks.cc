// gesher-base: the fork-lock registry (gesher/fork.h, ADR 0092).
#include <gesher/fork.h>

#include <errno.h>
#include <windows.h>
#include <winternl.h>

#include <atomic>
#include <cstddef>

namespace gesher {
namespace {

struct Entry {
  int order;
  const char* name;
  ForkLockGet get;
  const ForkLockOps* ops;
};

// Constant-initialised: registrations run from other files' static
// initialisers, in any order relative to this file's.
constexpr size_t kMaxEntries = 256;
SRWLOCK g_registry_lock = SRWLOCK_INIT;
Entry g_entries[kMaxEntries];
size_t g_count = 0;

// What the current fork holds (only the forking thread touches these, under
// g_registry_lock held shared).
void* g_taken[kMaxEntries];
const ForkLockOps* g_taken_ops[kMaxEntries];
size_t g_taken_count = 0;
void (*g_extra_release)() = nullptr;
bool g_extra_taken = false;

void ReleaseTaken() {
  if (g_extra_taken && g_extra_release) g_extra_release();
  g_extra_taken = false;
  while (g_taken_count > 0) {
    --g_taken_count;
    g_taken_ops[g_taken_count]->unlock(g_taken[g_taken_count]);
  }
}

// One round: true when everything is held.
//
// ntdll's loader lock is not taken: RtlCloneUserProcess first waits for the
// loader's work in progress (a thread leaving holds it until it has run its
// TLS and FLS callbacks under the loader lock), then takes the loader lock
// itself. Holding the loader lock here would stop that thread between the
// two and the clone with it. Instead, the callbacks Gesher runs as a thread
// leaves never wait for a lock of the fork set while a fork is in progress
// (ForkSetHeld): they defer their bookkeeping or read what the fork holds
// still.
bool TryRound(bool (*extra_try)()) {
  for (size_t i = 0; i < g_count; ++i) {
    void* lock = g_entries[i].get();
    if (lock == nullptr) continue;
    if (!g_entries[i].ops->try_lock(lock)) return false;
    g_taken[g_taken_count] = lock;
    g_taken_ops[g_taken_count] = g_entries[i].ops;
    ++g_taken_count;
  }
  if (extra_try != nullptr) {
    if (!extra_try()) return false;
    g_extra_taken = true;
  }
  return true;
}

// Set while a fork holds the whole set (from a successful round to release).
std::atomic<int> g_fork_set_held{0};

// The exit gate. A thread that Gesher started passes it as its start routine
// returns, before ntdll runs its TLS/FLS callbacks (other libraries' too:
// ART's thread teardown calls mprotect, under the loader lock), and records
// itself. A fork closes the gate, waits for the threads already past it to be
// gone, and only then takes the fork set: no thread can then be in its exit
// callbacks, waiting for a lock the fork holds, while the clone waits for it.
//
// A thread is recorded by its id and creation time, not by a handle: a
// process keeps no handle of a thread that left (a process that never forks
// holds exactly the handles it held before), and a reused id is told apart by
// its creation time.
SRWLOCK g_exit_gate = SRWLOCK_INIT;
SRWLOCK g_leaving_lock = SRWLOCK_INIT;  // never held across a wait
struct Leaving {
  DWORD tid;
  ULONGLONG created;
};
Leaving* g_leaving = nullptr;
size_t g_leaving_count = 0, g_leaving_capacity = 0;

ULONGLONG ThreadCreated(HANDLE thread) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// The recorded thread, while it has not ended: a handle to it, else nullptr.
HANDLE OpenLeaving(const Leaving& l) {
  HANDLE h = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, l.tid);
  if (h == nullptr) return nullptr;
  if (ThreadCreated(h) != l.created || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
    CloseHandle(h);
    return nullptr;
  }
  return h;
}

// Under g_leaving_lock: drops threads that are gone.
void PruneLeavingLocked() {
  size_t kept = 0;
  for (size_t i = 0; i < g_leaving_count; ++i) {
    if (HANDLE h = OpenLeaving(g_leaving[i])) {
      CloseHandle(h);
      g_leaving[kept++] = g_leaving[i];
    }
  }
  g_leaving_count = kept;
}

// With the gate closed: waits (bounded) for every thread past it to be gone.
// false when one is still there after 5 s (blocked in another library's exit
// callback, say): the fork must not take the set while it may still be inside
// the loader lock the clone waits for.
bool WaitForLeavingThreads() {
  const ULONGLONG deadline = GetTickCount64() + 5000;
  for (;;) {
    HANDLE batch[MAXIMUM_WAIT_OBJECTS];
    DWORD n = 0;
    AcquireSRWLockExclusive(&g_leaving_lock);
    size_t kept = 0;
    for (size_t i = 0; i < g_leaving_count; ++i) {
      HANDLE h = OpenLeaving(g_leaving[i]);
      if (h == nullptr) continue;
      g_leaving[kept++] = g_leaving[i];
      if (n < MAXIMUM_WAIT_OBJECTS) batch[n++] = h;
      else CloseHandle(h);
    }
    g_leaving_count = kept;
    ReleaseSRWLockExclusive(&g_leaving_lock);
    if (n == 0) return true;
    const ULONGLONG now = GetTickCount64();
    const DWORD r = now >= deadline ? WAIT_TIMEOUT
                                    : WaitForMultipleObjects(n, batch, TRUE, static_cast<DWORD>(deadline - now));
    for (DWORD i = 0; i < n; ++i) CloseHandle(batch[i]);
    if (r == WAIT_TIMEOUT) return false;
  }
}

}  // namespace

void RegisterForkLock(int order, const char* name, ForkLockGet get, const ForkLockOps* ops) {
  AcquireSRWLockExclusive(&g_registry_lock);
  if (g_count < kMaxEntries) {
    size_t at = g_count;
    while (at > 0 && g_entries[at - 1].order > order) {
      g_entries[at] = g_entries[at - 1];
      --at;
    }
    g_entries[at] = {order, name, get, ops};
    ++g_count;
  }
  ReleaseSRWLockExclusive(&g_registry_lock);
}

bool ForkSetHeld() { return g_fork_set_held.load(std::memory_order_acquire) != 0; }

std::mutex& ForkHelperLock() {
  static auto* lock = new std::mutex;
  return *lock;
}

namespace {
struct HelperStart {
  LPTHREAD_START_ROUTINE routine;
  void* argument;
  HANDLE started;
};
DWORD WINAPI RunHelper(void* raw) {
  HelperStart start = *static_cast<HelperStart*>(raw);
  SetEvent(start.started);
  return start.routine(start.argument);
}
}  // namespace

bool StartHelperThread(LPTHREAD_START_ROUTINE routine, void* argument, SIZE_T stack) {
  HelperStart start{routine, argument, CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (start.started == nullptr) return false;
  HANDLE thread = CreateThread(nullptr, stack, RunHelper, &start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (thread != nullptr) {
    WaitForSingleObject(start.started, INFINITE);
    CloseHandle(thread);
  }
  CloseHandle(start.started);
  return thread != nullptr;
}

// Whether this thread has passed the gate (ThreadLeaving runs once a thread).
thread_local bool t_left = false;

void ThreadLeaving() {
  if (t_left) return;
  t_left = true;
  AcquireSRWLockShared(&g_exit_gate);
  const Leaving self{GetCurrentThreadId(), ThreadCreated(GetCurrentThread())};
  AcquireSRWLockExclusive(&g_leaving_lock);
  if (g_leaving_count == g_leaving_capacity) PruneLeavingLocked();
  if (g_leaving_count == g_leaving_capacity) {
    const size_t capacity = g_leaving_capacity ? g_leaving_capacity * 2 : 64;
    auto* grown = static_cast<Leaving*>(
        g_leaving == nullptr ? HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(Leaving))
                             : HeapReAlloc(GetProcessHeap(), 0, g_leaving, capacity * sizeof(Leaving)));
    if (grown != nullptr) {
      g_leaving = grown;
      g_leaving_capacity = capacity;
    }
  }
  if (g_leaving_count < g_leaving_capacity) g_leaving[g_leaving_count++] = self;
  ReleaseSRWLockExclusive(&g_leaving_lock);
  ReleaseSRWLockShared(&g_exit_gate);
}

int AcquireForkLocks(bool (*extra_try)(), void (*extra_release)(), unsigned timeout_ms,
                     bool (*round_begin)(), void (*round_end)()) {
  AcquireSRWLockExclusive(&g_exit_gate);
  if (!WaitForLeavingThreads()) {
    // A leaving thread is stuck in its exit callbacks: no fork now.
    ReleaseSRWLockExclusive(&g_exit_gate);
    return errno = EAGAIN, -1;
  }
  AcquireSRWLockShared(&g_registry_lock);
  g_extra_release = extra_release;
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (unsigned round = 0;; ++round) {
    if (round_begin == nullptr || round_begin()) {
      if (TryRound(extra_try)) {
        g_fork_set_held.store(1, std::memory_order_seq_cst);
        return 0;
      }
      ReleaseTaken();
      if (round_end != nullptr) round_end();
    }
    if (GetTickCount64() >= deadline) {
      ReleaseSRWLockShared(&g_registry_lock);
      ReleaseSRWLockExclusive(&g_exit_gate);
      return errno = EAGAIN, -1;
    }
    // Let the holder go on: a few yields, then short sleeps.
    if (round < 16) SwitchToThread();
    else Sleep(1);
  }
}

void ReleaseForkLocks(bool child) {
  g_fork_set_held.store(0, std::memory_order_seq_cst);
  if (!child) {
    ReleaseTaken();
  } else {
    // The child: the locks are this thread's; the loader lock ntdll has freed.
    for (size_t i = g_taken_count; i-- > 0;) g_taken_ops[i]->child(g_taken[i]);
    g_taken_count = 0;
    g_extra_taken = false;
    InitializeSRWLock(&g_registry_lock);
    // The parent's leaving threads are not the child's.
    g_leaving_count = 0;
    InitializeSRWLock(&g_leaving_lock);
    InitializeSRWLock(&g_exit_gate);
    return;
  }
  ReleaseSRWLockShared(&g_registry_lock);
  ReleaseSRWLockExclusive(&g_exit_gate);
}

static_assert(sizeof(std::recursive_mutex) == sizeof(CRITICAL_SECTION),
              "std::recursive_mutex is a CRITICAL_SECTION (libc++ on Windows)");

void RecursiveMutexChild(void* lock) {
  auto* section = static_cast<CRITICAL_SECTION*>(lock);
  // Held by the forking thread (the round's try_lock added one level): the
  // levels below that one are outer frames of this same thread.
  const LONG depth = section->RecursionCount - 1;
  // A fresh section owned by the child's thread at that depth; the parent's
  // copy may record waiters (and a wait event) that are not the child's.
  InitializeCriticalSection(section);
  for (LONG i = 0; i < depth; ++i) EnterCriticalSection(section);
}

}  // namespace gesher

GESHER_FORK_LOCK(50, std::mutex, gesher::ForkHelperLock());
