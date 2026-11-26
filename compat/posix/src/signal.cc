// gesher-base: Linux signal API on Windows.
//
// Pending storage and synchronous waits are implemented here. sigsuspend
// supports ordinary one-argument handlers. Delivery needing an asynchronous
// machine frame/alternate stack or local job-control stop fails with ENOSYS,
// without consuming the pending record. See the capability notes in
// docs/research/realtime-signals-2026-11-17.md.
// Synchronous Windows exceptions are handled separately in fault_signals.cc.

#include <signal.h>
#include <sys/resource.h>
#include <gesher/pending_signals.h>

#include <errno.h>
#include <windows.h>

#if defined(__has_include)
#if __has_include(<pthread.h>)
#include <pthread.h>
#else
typedef void* pthread_t;
#endif
#if __has_include(<unistd.h>)
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cstddef>
#include <mutex>
#include <new>
#include <gesher/fork.h>

// The underlying winpthreads functions; callers use the POSIX adapters.
#undef pthread_create
#undef pthread_kill

extern "C" int gesher_kill_other(pid_t pid, int sig);

namespace {

constexpr int kMaxSignal = NSIG - 1;
std::atomic<size_t> queue_count{0};
struct LocalPending;

struct State {
  std::mutex lock;
  struct sigaction actions[NSIG] = {};
  gesher::signals::Pending<siginfo_t> pending;
  LocalPending* local_pending = nullptr;
  std::condition_variable sent;
};

State& state() {
  static State* s = new State();
  return *s;
}

thread_local stack_t alt_stack = {nullptr, SS_DISABLE, 0};

// The registry owns no thread references. Each registered queue is linked
// until that thread exits, even if the thread is detached or cancelled.
//
// A thread's queue is unlinked as the thread leaves, in a thread_local
// destructor (under ntdll's loader lock). A fork's clone waits for that lock
// while the fork holds state().lock (gesher/fork.h): then the queue is only
// marked dead, kept allocated, and unlinked and freed by the next thread that
// registers one.
struct LocalPending {
  gesher::signals::Pending<siginfo_t> pending;
  sigset_t mask = 0;
  pthread_t thread{};
  LocalPending* next = nullptr;
  bool registered = false;
  bool dead = false;            // its thread left while a fork held the lock
  void (*release)(LocalPending*) = nullptr;  // frees its owner once unlinked

  void RegisterLocked(State& state, pthread_t id);
};

// Under state().lock: unlinks and frees the queues of threads that left.
void SweepDeadLocked(State& state) {
  for (LocalPending** entry = &state.local_pending; *entry != nullptr;) {
    LocalPending* p = *entry;
    if (!p->dead) {
      entry = &p->next;
      continue;
    }
    *entry = p->next;
    if (p->release != nullptr) p->release(p);
  }
}

void LocalPending::RegisterLocked(State& state, pthread_t id) {
  SweepDeadLocked(state);
  thread = id;
  next = state.local_pending;
  state.local_pending = this;
  registered = true;
}

// As its thread leaves: true when `p` is unlinked (its owner may be freed);
// false when it was marked dead instead (its owner must stay allocated).
bool UnlinkOrRetire(LocalPending* p) {
  if (!p->registered) return true;
  State& s = state();
  if (!gesher::LockUnlessForkHolds(s.lock)) {
    p->dead = true;  // the fork holds the lock: no reader runs until it is done
    return false;
  }
  auto** entry = &s.local_pending;
  while (*entry != p) entry = &(*entry)->next;
  *entry = p->next;
  s.lock.unlock();
  return true;
}

struct ThreadStart {
  void* (*start)(void*);
  void* argument;
  LocalPending signals;
};
void FreeThreadStart(LocalPending* p) {
  delete reinterpret_cast<ThreadStart*>(reinterpret_cast<char*>(p) - offsetof(ThreadStart, signals));
}
struct ThreadStartRelease {
  void operator()(ThreadStart* start) const {
    start->signals.release = &FreeThreadStart;
    if (UnlinkOrRetire(&start->signals)) delete start;
  }
};
thread_local std::unique_ptr<ThreadStart, ThreadStartRelease> thread_start;

// A thread not started here: its queue is allocated, as it may outlive the
// thread's own storage (see above).
void FreeLocalPending(LocalPending* p) { delete p; }
struct LocalPendingHolder {
  LocalPending* queue = nullptr;
  ~LocalPendingHolder() {
    if (queue != nullptr && UnlinkOrRetire(queue)) delete queue;
  }
};
thread_local LocalPendingHolder local_pending_holder;

LocalPending& CurrentPending() {
  if (thread_start) return thread_start->signals;
  LocalPending*& queue = local_pending_holder.queue;
  if (queue == nullptr) {
    queue = new LocalPending();
    queue->release = &FreeLocalPending;
  }
  if (!queue->registered) {
    const pthread_t self = pthread_self();
    State& current = state();
    std::lock_guard<std::mutex> guard(current.lock);
    queue->RegisterLocked(current, self);
  }
  return *queue;
}

// A thread that ends by pthread_exit (winpthreads' own, which longjmps out of
// the start routine) passes the fork's exit gate too: winpthreads runs the
// thread's key destructors on both ways out, before the thread's exit
// callbacks.
void LeavingKeyDestructor(void*) { gesher::ThreadLeaving(); }
// The key, or nullptr if none could be made (out of keys): such threads then
// pass the gate only when their start routine returns.
const pthread_key_t* LeavingKey() {
  static pthread_key_t key{};
  static const bool made = pthread_key_create(&key, LeavingKeyDestructor) == 0;
  return made ? &key : nullptr;
}
void PassGateOnExit() {
  if (const pthread_key_t* key = LeavingKey()) pthread_setspecific(*key, reinterpret_cast<void*>(1));
}

void* StartThread(void* argument) {
  PassGateOnExit();
  {
    // The creator still owns the allocation until it publishes the returned
    // pthread identity and its inherited mask. User code may start after that.
    std::lock_guard<std::mutex> guard(state().lock);
    thread_start.reset(static_cast<ThreadStart*>(argument));
  }
  void* result = thread_start->start(thread_start->argument);
  gesher::ThreadLeaving();  // before its exit callbacks (gesher/fork.h)
  return result;
}

// A thread with no inherited mask: only the fork's exit gate.
struct PlainStart {
  void* (*start)(void*);
  void* argument;
};
void* StartPlainThread(void* raw) {
  PlainStart plain = *static_cast<PlainStart*>(raw);
  delete static_cast<PlainStart*>(raw);
  PassGateOnExit();
  void* result = plain.start(plain.argument);
  gesher::ThreadLeaving();  // before its exit callbacks (gesher/fork.h)
  return result;
}

bool Valid(int signum) { return signum >= 1 && signum <= kMaxSignal; }
sigset_t Bit(int signum) { return static_cast<sigset_t>(1) << (signum - 1); }

bool DefaultIgnores(int sig) {
  return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

bool DefaultStops(int sig) {
  // The host CRT retains SIGBREAK/SIGABRT at 21/22. Those are not relabeled
  // here; the bionic adapter has Android's complete separate signal ABI.
  return sig == SIGSTOP || sig == SIGTSTP;
}

bool Ignored(const struct sigaction& action, int sig) {
  return action.sa_handler == SIG_IGN ||
         (action.sa_handler == SIG_DFL && DefaultIgnores(sig));
}

void PopSignalLocked(gesher::signals::Pending<siginfo_t>& queue, int signum, siginfo_t* info) {
  if (!queue.Pop(signum, info) && info != nullptr) info->si_signo = signum;
}

bool PushSignalLocked(gesher::signals::Pending<siginfo_t>& queue, int signum, int code,
                      const union sigval* value) {
  siginfo_t info = {};
  info.si_signo = signum;
  info.si_code = code;
  info.si_pid = getpid();
  info.si_uid = getuid();
  if (value != nullptr) info.si_value = *value;
  return queue.Push(signum, code, info);
}

// Makes `signum` pending for the process. Signal 0 only checks the target.
int Send(pid_t pid, int signum, int code, const union sigval* value, bool to_thread = false) {
  if (signum != 0 && !Valid(signum)) return errno = EINVAL, -1;
  if (pid != getpid() && pid != 0) {
    // The existing interprocess mailbox is a standard-signal bitmap. It
    // cannot carry queued values or RT multiplicity, and must not accept
    // those requests as an ordinary kill that silently changes their input.
    if (signum != 0 && (signum >= 32 || code == SI_QUEUE)) return errno = ENOSYS, -1;
    // Another process: through its mailbox (process.cc).
    return gesher_kill_other(pid, signum);
  }
  if (signum == 0) return 0;
  auto& own = CurrentPending();
  State& s = state();
  {
    std::lock_guard<std::mutex> guard(s.lock);
    // A blocked ignored signal remains waitable; installing SIG_IGN below
    // flushes pending instances irrespective of the mask.
    if ((own.mask & Bit(signum)) == 0 && Ignored(s.actions[signum], signum)) return 0;
    if ((own.mask & Bit(signum)) == 0 && s.actions[signum].sa_handler == SIG_DFL &&
        DefaultStops(signum)) return errno = ENOSYS, -1;
    if (!PushSignalLocked(to_thread ? own.pending : s.pending, signum, code, value)) return errno = EAGAIN, -1;
  }
  s.sent.notify_all();
  return 0;
}

// Takes the lowest pending signal in `set`, waiting until `deadline` (none:
// forever). Returns the signal, or -1 with errno EAGAIN when it times out.
int Take(const sigset_t* set, siginfo_t* info,
         const std::chrono::steady_clock::time_point* deadline) {
  State& s = state();
  auto& own = CurrentPending().pending;
  std::unique_lock<std::mutex> guard(s.lock);
  auto ready = [&] { return ((own.Mask() | s.pending.Mask()) & *set) != 0; };
  if (deadline == nullptr) {
    s.sent.wait(guard, ready);
  } else if (!s.sent.wait_until(guard, *deadline, ready)) {
    return errno = EAGAIN, -1;
  }
  for (int signum = 1; signum <= kMaxSignal; ++signum) {
    if (((own.Mask() | s.pending.Mask()) & *set & Bit(signum)) != 0) {
      PopSignalLocked((own.Mask() & Bit(signum)) != 0 ? own : s.pending, signum, info);
      return signum;
    }
  }
  return errno = EAGAIN, -1;  // not reached: ready() held
}

}  // namespace

extern "C" {

// This process owns both adapters' queue records. RLIMIT_SIGPENDING is read
// at each allocation, so lowering it affects subsequent sends immediately.
// Cross-process, real-UID-wide charging is still an explicit N20 blocker.
bool gesher_signal_queue_acquire(bool enforce_limit) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_SIGPENDING, &limit) != 0) return false;
  size_t count = queue_count.load(std::memory_order_relaxed);
  do {
    if (enforce_limit && count >= limit.rlim_cur) return false;
  } while (!queue_count.compare_exchange_weak(
      count, count + 1, std::memory_order_relaxed));
  return true;
}

void gesher_signal_queue_release() {
  queue_count.fetch_sub(1, std::memory_order_relaxed);
}

int sigemptyset(sigset_t* set) {
  *set = 0;
  return 0;
}

int sigfillset(sigset_t* set) {
  *set = ~static_cast<sigset_t>(0);
  return 0;
}

int sigaddset(sigset_t* set, int signum) {
  if (!Valid(signum)) return errno = EINVAL, -1;
  *set |= Bit(signum);
  return 0;
}

int sigdelset(sigset_t* set, int signum) {
  if (!Valid(signum)) return errno = EINVAL, -1;
  *set &= ~Bit(signum);
  return 0;
}

int sigismember(const sigset_t* set, int signum) {
  if (!Valid(signum)) return errno = EINVAL, -1;
  return (*set & Bit(signum)) != 0;
}

// The raw implementations. sigaction() and sigprocmask() below are weak, so
// that libsigchain, which interposes them as it does on Linux, can define
// them and reach these through gesher_*_raw.
int gesher_sigaction_raw(int signum, const struct sigaction* act, struct sigaction* oldact) {
  if (!Valid(signum) || (act != nullptr && (signum == SIGKILL || signum == SIGSTOP))) {
    return errno = EINVAL, -1;
  }
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  if (oldact != nullptr) *oldact = s.actions[signum];
  if (act != nullptr) {
    s.actions[signum] = *act;
    s.actions[signum].sa_mask &= ~(Bit(SIGKILL) | Bit(SIGSTOP));
    if (Ignored(*act, signum)) {
      s.pending.Discard(signum);
      for (auto* thread = s.local_pending; thread != nullptr; thread = thread->next)
        thread->pending.Discard(signum);
    }
  }
  return 0;
}

int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
  auto& own = CurrentPending();
  std::lock_guard<std::mutex> guard(state().lock);
  if (oldset != nullptr) *oldset = own.mask;
  if (set == nullptr) return 0;
  switch (how) {
    case SIG_BLOCK: own.mask |= *set; break;
    case SIG_UNBLOCK: own.mask &= ~*set; break;
    case SIG_SETMASK: own.mask = *set; break;
    default: return EINVAL;
  }
  own.mask &= ~(Bit(SIGKILL) | Bit(SIGSTOP));
  return 0;
}

int gesher_sigprocmask_raw(int how, const sigset_t* set, sigset_t* oldset) {
  int result = pthread_sigmask(how, set, oldset);
  return result == 0 ? 0 : (errno = result, -1);
}

__attribute__((weak)) int sigaction(int signum, const struct sigaction* act,
                                    struct sigaction* oldact) {
  return gesher_sigaction_raw(signum, act, oldact);
}

__attribute__((weak)) int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
  return gesher_sigprocmask_raw(how, set, oldset);
}

int sigaltstack(const stack_t* ss, stack_t* old_ss) {
  if (old_ss != nullptr) *old_ss = alt_stack;
  if (ss != nullptr) {
    if ((ss->ss_flags & SS_DISABLE) == 0 && ss->ss_size < MINSIGSTKSZ) {
      return errno = ENOMEM, -1;
    }
    alt_stack = *ss;
  }
  return 0;
}

int sigpending(sigset_t* set) {
  State& s = state();
  auto& own = CurrentPending();
  std::lock_guard<std::mutex> guard(s.lock);
  *set = (own.pending.Mask() | s.pending.Mask()) & own.mask;
  return 0;
}

int kill(pid_t pid, int sig) { return Send(pid, sig, SI_USER, nullptr); }

int tgkill(pid_t tgid, pid_t tid, int sig) {
  if (sig != 0 && !Valid(sig)) return errno = EINVAL, -1;
  if (tgid <= 0 || tid <= 0) return errno = EINVAL, -1;
  if (tgid != getpid() || tid != static_cast<pid_t>(GetCurrentThreadId()))
    return errno = ENOSYS, -1;
  return Send(tgid, sig, SI_TKILL, nullptr, true);
}

int gesher_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                         void* (*start)(void*), void* argument) {
  // Thread masks are written only by their own thread. An empty inherited
  // mask needs no private state until the new thread actually uses signals.
  const sigset_t inherited = thread_start ? thread_start->signals.mask
                                          : local_pending_holder.queue != nullptr ? local_pending_holder.queue->mask
                                                                                  : 0;
  if (inherited == 0) {
    auto* plain = new (std::nothrow) PlainStart{start, argument};
    if (plain == nullptr) return EAGAIN;
    const int error = pthread_create(thread, attr, StartPlainThread, plain);
    if (error != 0) delete plain;
    return gesher_errno_from_crt(error);
  }
  std::unique_ptr<ThreadStart> context(new (std::nothrow) ThreadStart{start, argument, {}});
  if (!context) return EAGAIN;
  context->signals.mask = inherited;
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  const int error = pthread_create(thread, attr, StartThread, context.get());
  if (error != 0) return gesher_errno_from_crt(error);
  context->signals.RegisterLocked(s, *thread);
  context.release();
  return 0;
}

int gesher_pthread_kill(pthread_t thread, int sig) {
  // winpthreads checks that the thread exists (and handles signal 0).
  const int rc = gesher_errno_from_crt(pthread_kill(thread, 0));
  if (rc != 0 || sig == 0) return rc;
  if (!Valid(sig)) return EINVAL;
  if (pthread_equal(thread, pthread_self())) return Send(getpid(), sig, SI_TKILL, nullptr, true) == 0 ? 0 : errno;

  State& s = state();
  {
    std::unique_lock<std::mutex> guard(s.lock);
    auto* target = s.local_pending;
    while (target && (target->dead || !pthread_equal(target->thread, thread))) target = target->next;
    if (!target) {
      guard.unlock();
      const int status = gesher_errno_from_crt(pthread_kill(thread, 0));
      return status == 0 ? ENOSYS : status;
    }
    // Asynchronous delivery needs the target's machine frame. A blocked
    // signal can be queued now, even before its target starts sigwaitinfo.
    if ((target->mask & Bit(sig)) == 0) return ENOSYS;
    if (!PushSignalLocked(target->pending, sig, SI_TKILL, nullptr)) return EAGAIN;
  }
  s.sent.notify_all();
  return 0;
}

int sigqueue(pid_t pid, int sig, const union sigval value) {
  return Send(pid, sig, SI_QUEUE, &value);
}

int sigwaitinfo(const sigset_t* set, siginfo_t* info) { return Take(set, info, nullptr); }

int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout) {
  if (timeout == nullptr) return Take(set, info, nullptr);
  if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) return errno = EINVAL, -1;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout->tv_sec) +
                  std::chrono::nanoseconds(timeout->tv_nsec);
  return Take(set, info, &deadline);
}

int sigwait(const sigset_t* set, int* sig) {
  int signum = Take(set, nullptr, nullptr);
  if (signum < 0) return errno;
  *sig = signum;
  return 0;
}

int sigsuspend(const sigset_t* mask) {
  if (mask == nullptr) return errno = EFAULT, -1;

  auto& local = CurrentPending();
  sigset_t temp_mask = *mask & ~(Bit(SIGKILL) | Bit(SIGSTOP));
  sigset_t unmasked = ~temp_mask;

  State& s = state();
  auto& own = local.pending;
  std::unique_lock<std::mutex> guard(s.lock);
  sigset_t old_mask = local.mask;
  local.mask = temp_mask;

  siginfo_t delivered_info = {};
  struct sigaction delivered_act = {};
  int delivered_sig = 0;

  for (;;) {
    int candidate = 0;
    for (int signum = 1; signum <= kMaxSignal; ++signum) {
      if (((own.Mask() | s.pending.Mask()) & unmasked & Bit(signum)) != 0) {
        candidate = signum;
        break;
      }
    }

    if (candidate != 0) {
      struct sigaction act = s.actions[candidate];
      auto& queue = (own.Mask() & Bit(candidate)) != 0 ? own : s.pending;
      // Check if signal action is to ignore
      if (act.sa_handler == SIG_IGN ||
          (act.sa_handler == SIG_DFL &&
           (candidate == SIGCHLD || candidate == SIGURG ||
            candidate == SIGWINCH || candidate == SIGCONT))) {
        // Discard ignored signal
        PopSignalLocked(queue, candidate, nullptr);
        continue;
      }

      // Check if signal action is default terminate
      if (act.sa_handler == SIG_DFL) {
        if (DefaultStops(candidate)) {
          local.mask = old_mask;
          return errno = ENOSYS, -1;
        }
        PopSignalLocked(queue, candidate, &delivered_info);
        local.mask = old_mask;
        guard.unlock();
        // A forked child ends as a signal kills it (gesher/fork.h).
        if (gesher::InForkedChild()) TerminateProcess(GetCurrentProcess(), gesher::SignalExitCode(candidate));
        ExitProcess(128 + static_cast<UINT>(candidate));
      }

      // Action is a user-defined handler
      if ((act.sa_flags & SA_SIGINFO) != 0 ||
          ((act.sa_flags & SA_ONSTACK) != 0 && (alt_stack.ss_flags & SS_DISABLE) == 0)) {
        local.mask = old_mask;
        return errno = ENOSYS, -1;
      }
      PopSignalLocked(queue, candidate, &delivered_info);
      delivered_act = act;
      delivered_sig = candidate;
      if ((delivered_act.sa_flags & SA_RESETHAND) != 0) {
        s.actions[candidate] = {};
      }
      break;
    }

    s.sent.wait(guard);
  }

  sigset_t handler_mask = temp_mask | delivered_act.sa_mask;
  if ((delivered_act.sa_flags & SA_NODEFER) == 0) {
    handler_mask |= Bit(delivered_sig);
  }
  local.mask = handler_mask;
  guard.unlock();

  if (delivered_act.sa_handler != nullptr &&
      delivered_act.sa_handler != SIG_DFL &&
      delivered_act.sa_handler != SIG_IGN) {
    delivered_act.sa_handler(delivered_sig);
  }

  guard.lock();
  local.mask = old_mask;
  return errno = EINTR, -1;
}

}  // extern "C"

// fork (gesher/fork.h): gesher-base's signal state in a forked child.
GESHER_FORK_LOCK(gesher::kForkOrderProcess, std::mutex, state().lock);
GESHER_FORK_LOCK(gesher::kForkOrderLeaf, std::condition_variable, state().sent);
