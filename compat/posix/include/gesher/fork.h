// gesher: a real process clone for fork(2) (docs/decisions/0092-arm64-guest-fork.md).
//
// gesher::CloneProcess makes a child process with ntdll's RtlCloneUserProcess:
// a copy-on-write copy of this address space, with only the calling thread.
// Both processes return from it.
//
// The fork-lock registry: every lock a system call path of gesher-base, the
// bionic shim or the ELF loader can take registers here (GESHER_FORK_LOCK).
// AcquireForkLocks takes all of them, so that no other thread is inside any
// of them while the process is cloned: the child's copies of what they guard
// are consistent, and none is held by a thread the child does not have. Each
// lock is only tried, never waited for: when one is busy, everything taken so
// far is released and the round starts again, so a thread that holds one lock
// and waits for another can always go on. RtlCloneUserProcess then waits for
// threads that are leaving (their TLS/FLS callbacks under ntdll's loader
// lock): those callbacks never wait for the fork set (ForkSetHeld). In the
// child each lock is made anew (or, for a recursive lock the forking thread
// itself held, handed to the child's thread).
#pragma once

#include <windows.h>

#include <mutex>
#include <new>
#include <shared_mutex>
#include <condition_variable>

namespace gesher {

// 1 in the parent (with the child's process handle and pid), 0 in the child,
// -1 with errno set if no child was made. Every handle of this process that a
// child can use is inherited, at the same value, then given back its own
// inheritance flag in both processes (fork keeps close-on-exec descriptors).
// Handles bound to this process's thread pool and I/O completion ports are
// not inherited (the child has no thread pool; see InForkedChild). In the
// parent, the child's signal mailbox exists before this returns, so a signal
// sent to the new pid at once is delivered to it. The caller holds the fork
// set (AcquireForkLocks).
int CloneProcess(HANDLE* process, DWORD* pid);

// Gesher's spawns hold this (shared) around CreateProcess; a fork holds it
// exclusively while every handle of the process is inheritable. A process
// another component starts in that window with bInheritHandles and no handle
// list would inherit them all (documented in ADR 0092).
void AcquireSpawnLock();
void ReleaseSpawnLock();

// Process creation for forked children (spawn_broker.cc): a clone cannot call
// CreateProcess (it lacks the CSR shared data kernelbase reads). The first
// process that forks (the root) serves requests (StartSpawnBroker, before its
// first fork); a forked child's BrokeredCreateProcess has it create the
// process with the child as its parent (PROC_THREAD_ATTRIBUTE_PARENT_PROCESS):
// its handles, standard handles and parent pid are the child's. Returns a
// Win32 error: ERROR_NOT_SUPPORTED (ENOTSUP) once the root has ended or its
// broker has stopped, ERROR_BAD_LENGTH (E2BIG) for a request over 1 MiB.
void StartSpawnBroker();
DWORD BrokeredCreateProcess(const wchar_t* application, const wchar_t* command_line, const wchar_t* environment,
                            const wchar_t* cwd, DWORD flags, const STARTUPINFOW& startup,
                            const HANDLE* inherit, size_t inherit_count, PROCESS_INFORMATION* info);
// The broker serves only forked descendants of its root: each fork records
// its child here (pid and creation time, in memory the root's tree shares),
// and a reaped child is taken out.
void RegisterForkedChild(DWORD pid, HANDLE process);
void ForgetForkedChild(DWORD pid);
// The broker's own pipe handles, which a clone must not inherit (CloneProcess).
bool IsBrokerHandle(HANDLE handle);

// A program that replaces this process (exec in a process that uses the forked
// child's exit codes): signals sent to this process go to it instead, 0 to stop.
void SetExecForward(DWORD pid);

// Releases the parent's references to a reaped child's mailbox.
void ForgetChildMailbox(DWORD pid);

// Whether this process holds a memfd write lease (a writable shared mapping of
// a memfd). The caller holds the fork set. A forked child takes its own lease
// for each one it inherits (mman.cc); the parent waits for that before it
// lets another thread unmap and seal (ADR 0092).
bool ForkHasMemfdWriteLeases();

// True in a process CloneProcess made. The ntdll thread pool of the parent
// does not exist there: code that would use it takes another path or fails.
bool InForkedChild();
// In a forked child: whether `handle` is one it inherited at the clone (the
// value was open in it when the clone returned). A handle value the parent
// stored but opened after its handles were made inheritable is not, and may
// name another of the child's handles later: such a value is never closed.
bool InheritedAtFork(HANDLE handle);

// In the child, after CloneProcess: gesher-base's per-process state named by
// the pid (the signal mailbox other processes' kill() reaches) is opened for
// the child's pid and watched by a thread of its own. False when that could
// not be done (the child would lose signals): the fork must not go on.
bool ReinitializeAfterFork();

// A forked child's death by a signal, as its Windows exit code. Ordinary exit
// statuses of a forked child are 0..255, so this cannot be confused with
// exit(128 + n); Gesher's wait decodes it (exec_spawn's WaitStatus).
constexpr DWORD kForkSignalExitBase = 0x0F5E0000;
inline DWORD ForkSignalExitCode(int sig) { return kForkSignalExitBase | static_cast<DWORD>(sig & 0x7f); }
// The exit code a process ended by `sig` gets: the marker above in a forked
// child and in every process whose parent uses these codes (a program a
// forked child exec'd or spawned: its parent decodes its status so), 128 + sig
// elsewhere (Windows callers of a Gesher program read that). A process learns
// it from its parent's mailbox at startup.
DWORD SignalExitCode(int sig);
// Whether this process uses those codes; it also decodes its own children's
// statuses with them (ForkedWaitStatusOf).
bool UsesForkExitCodes();
// The Linux status word of a process that uses the forked child's exit codes.
inline int ForkedWaitStatusOf(DWORD code, int (*windows_status)(DWORD)) {
  if ((code & 0xFFFF0000u) == kForkSignalExitBase) return static_cast<int>(code & 0x7f);
  if (code > 0xff) return windows_status(code);  // an exception (crash) code
  return static_cast<int>(code & 0xff) << 8;
}
// Whether the process `pid` is a forked child (its mailbox says so).
bool IsForkedChild(DWORD pid);

// --- the fork-lock registry -------------------------------------------------------

using ForkLockGet = void* (*)();
struct ForkLockOps {
  bool (*try_lock)(void* lock);
  void (*unlock)(void* lock);  // in the parent, and when a round is abandoned
  void (*child)(void* lock);   // in the child, which holds it
};
// `order`: smaller ones are taken first (see kForkOrder*). `get` returns the
// lock, or nullptr while it does not exist yet (a lazily made table).
void RegisterForkLock(int order, const char* name, ForkLockGet get, const ForkLockOps* ops);

// Orders, outermost first; within one, registration order.
constexpr int kForkOrderLoader = 100;      // the ELF loader
constexpr int kForkOrderDescriptors = 200; // descriptor tables, guards
constexpr int kForkOrderProcess = 300;     // children, popen, signals, threads
constexpr int kForkOrderSubsystem = 400;   // environment, properties, epoll, mman...
constexpr int kForkOrderLeaf = 500;        // leaf locks, condition variables

// Closes the exit gate (ThreadLeaving), waits for the threads already past it
// to be gone, then takes every registered lock; `extra_try` (the
// translator's, taken last) is tried the same way and `extra_release` undoes
// it. Returns 0, or -1 with errno EAGAIN when some lock stayed busy for the
// whole of `timeout_ms`. `round_begin`, when given, runs at the start of each
// round, before any lock of the set is tried: it may take a lock outside the
// set that is taken before the set's locks, waiting for it only briefly, and
// returns false without it (the round then fails like one that found a lock
// of the set busy). `round_end` undoes it after a round that failed past it.
// After a successful round the caller undoes it.
int AcquireForkLocks(bool (*extra_try)(), void (*extra_release)(), unsigned timeout_ms,
                     bool (*round_begin)() = nullptr, void (*round_end)() = nullptr);
// After the clone: the parent releases them, the child makes them anew.
void ReleaseForkLocks(bool child);

// While a fork holds the whole fork set (and is cloning). Code that runs as a
// thread leaves (TLS and FLS callbacks, under ntdll's loader lock, which the
// clone waits for) must not wait for a lock of the set then: it defers its
// work, or reads what the fork holds still (nothing can change it meanwhile).
bool ForkSetHeld();

// Called by a thread Gesher started as its start routine returns (or it calls
// pthread_exit), before ntdll runs its exit callbacks: a fork in progress
// holds it here, and a fork waits for the threads past this point to be gone
// before it takes the fork set (fork_locks.cc).
void ThreadLeaving();

// Gesher's own helper threads (a forked child's mailbox and child watchers,
// the spawn broker) hold this while they work, so that a fork never clones
// one in the middle of its work (inside the heap, say): it is in the fork set.
std::mutex& ForkHelperLock();
// Starts a helper thread and returns once it runs its own code (its loader
// initialisation, which allocates, is over), with its id in `thread_id` when
// given. false if it could not start or, in a forked child, did not get
// there within 5 s (logged, ERROR_TIMEOUT; it then never runs `routine`).
bool StartHelperThread(LPTHREAD_START_ROUTINE routine, void* argument, SIZE_T stack,
                       DWORD* thread_id = nullptr);

// For code that runs as a thread leaves: takes `lock`, or returns false
// without it while a fork holds the whole set (the caller then defers what it
// would change, or reads without the lock). While a fork only tries the set,
// the lock comes free at once.
template <typename Lock>
bool LockUnlessForkHolds(Lock& lock) {
  while (!lock.try_lock()) {
    if (ForkSetHeld()) return false;
    SwitchToThread();
  }
  return true;
}

// --- operations for the common lock types -----------------------------------------

template <typename Lock>
struct ForkLockTraits;

template <>
struct ForkLockTraits<std::mutex> {
  static bool TryLock(void* l) { return static_cast<std::mutex*>(l)->try_lock(); }
  static void Unlock(void* l) { static_cast<std::mutex*>(l)->unlock(); }
  static void Child(void* l) { new (l) std::mutex(); }
};
template <>
struct ForkLockTraits<std::shared_mutex> {
  static bool TryLock(void* l) { return static_cast<std::shared_mutex*>(l)->try_lock(); }
  static void Unlock(void* l) { static_cast<std::shared_mutex*>(l)->unlock(); }
  static void Child(void* l) { new (l) std::shared_mutex(); }
};
template <>
struct ForkLockTraits<SRWLOCK> {
  static bool TryLock(void* l) { return TryAcquireSRWLockExclusive(static_cast<SRWLOCK*>(l)) != 0; }
  static void Unlock(void* l) { ReleaseSRWLockExclusive(static_cast<SRWLOCK*>(l)); }
  static void Child(void* l) { InitializeSRWLock(static_cast<SRWLOCK*>(l)); }
};
// std::recursive_mutex is a CRITICAL_SECTION (libc++ on Windows). The forking
// thread may itself hold it in outer frames (a fork from a library
// constructor, under the ELF loader's lock): the child's copy is rebuilt
// owned by the child's thread at that same depth, so those frames' guards
// release it normally.
void RecursiveMutexChild(void* lock);
template <>
struct ForkLockTraits<std::recursive_mutex> {
  static bool TryLock(void* l) { return static_cast<std::recursive_mutex*>(l)->try_lock(); }
  static void Unlock(void* l) { static_cast<std::recursive_mutex*>(l)->unlock(); }
  static void Child(void* l) { RecursiveMutexChild(l); }
};
// A condition variable a parent's thread may wait on: only made anew in the child.
template <>
struct ForkLockTraits<std::condition_variable> {
  static bool TryLock(void*) { return true; }
  static void Unlock(void*) {}
  static void Child(void* l) { new (l) std::condition_variable(); }
};

template <typename Lock>
const ForkLockOps* ForkLockOpsFor() {
  static const ForkLockOps ops = {&ForkLockTraits<Lock>::TryLock, &ForkLockTraits<Lock>::Unlock,
                                  &ForkLockTraits<Lock>::Child};
  return &ops;
}

struct ForkLockRegistration {
  ForkLockRegistration(int order, const char* name, ForkLockGet get, const ForkLockOps* ops) {
    RegisterForkLock(order, name, get, ops);
  }
};

}  // namespace gesher

#define GESHER_FORK_CAT2(a, b) a##b
#define GESHER_FORK_CAT(a, b) GESHER_FORK_CAT2(a, b)
// GESHER_FORK_LOCK(order, Type, expression): registers the lock `expression`
// (an lvalue of Type, evaluated at fork time) in the fork set.
#define GESHER_FORK_LOCK(order, Type, expr)                                                \
  static const ::gesher::ForkLockRegistration GESHER_FORK_CAT(g_fork_lock_, __LINE__)(     \
      (order), #expr, []() -> void* { return static_cast<void*>(&(expr)); },              \
      ::gesher::ForkLockOpsFor<Type>())
// As above, with `pointer_expression` a Type* or nullptr (not made yet).
#define GESHER_FORK_LOCK_PTR(order, Type, pointer_expression)                              \
  static const ::gesher::ForkLockRegistration GESHER_FORK_CAT(g_fork_lock_, __LINE__)(     \
      (order), #pointer_expression,                                                       \
      []() -> void* { return static_cast<void*>(pointer_expression); },                   \
      ::gesher::ForkLockOpsFor<Type>())
