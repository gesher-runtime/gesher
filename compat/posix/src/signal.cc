// gesher-base: Linux signal API on Windows.
//
// Status: signal sets, masks and handler registration work. Signals sent to
// this process (kill, tgkill, pthread_kill, sigqueue) are kept pending until
// a thread takes them with sigwait, sigwaitinfo or sigtimedwait, as ART's
// signal catcher does with SIGQUIT and SIGUSR1; handlers are not run for
// them. Delivery of synchronous signals from Windows exceptions is the next
// step (needs the CONTEXT <-> ucontext_t translation from <sys/ucontext.h>).

#include <signal.h>

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
#include <deque>
#include <mutex>

// winpthreads' own, which gesher_pthread_kill calls; <pthread.h> routes
// everyone else to gesher_pthread_kill.
#undef pthread_kill
#if !defined(__MINGW32__)
static inline int pthread_kill(pthread_t, int) { return 0; }
#endif

extern "C" int gesher_kill_other(pid_t pid, int sig);

namespace {

constexpr int kMaxSignal = NSIG - 1;
constexpr int kRealtimeSignalMin = 32;

#ifndef SIGQUEUE_MAX
#define SIGQUEUE_MAX 32
#endif

struct State {
  std::mutex lock;
  struct sigaction actions[NSIG] = {};
  // Signals sent to the process and not taken yet; Linux keeps one of each
  // standard signal pending, as here. Real-time signals (>= 32) maintain
  // non-coalesced FIFO queues.
  sigset_t pending = 0;
  siginfo_t info[NSIG] = {};
  std::deque<siginfo_t> rt_queues[NSIG];
  size_t rt_queue_total = 0;
  std::condition_variable sent;
};

State& state() {
  static State* s = new State();
  return *s;
}

thread_local sigset_t thread_mask = 0;
thread_local stack_t alt_stack = {nullptr, SS_DISABLE, 0};

bool Valid(int signum) { return signum >= 1 && signum <= kMaxSignal; }
sigset_t Bit(int signum) { return static_cast<sigset_t>(1) << (signum - 1); }

void PopSignalLocked(State& s, int signum, siginfo_t* info) {
  if (signum < kRealtimeSignalMin) {
    s.pending &= ~Bit(signum);
    if (info != nullptr) *info = s.info[signum];
  } else {
    if (!s.rt_queues[signum].empty()) {
      if (info != nullptr) *info = s.rt_queues[signum].front();
      s.rt_queues[signum].pop_front();
      s.rt_queue_total--;
      if (s.rt_queues[signum].empty()) {
        s.pending &= ~Bit(signum);
      }
    } else {
      s.pending &= ~Bit(signum);
    }
  }
}

// Makes `signum` pending for the process. Signal 0 only checks the target.
int Send(pid_t pid, int signum, int code, const union sigval* value) {
  if (signum != 0 && !Valid(signum)) return errno = EINVAL, -1;
  if (pid != getpid() && pid != 0) {
    // Another process: through its mailbox (process.cc).
    return gesher_kill_other(pid, signum);
  }
  if (signum == 0) return 0;
  State& s = state();
  {
    std::lock_guard<std::mutex> guard(s.lock);
    if (signum < kRealtimeSignalMin) {
      if ((s.pending & Bit(signum)) == 0) {
        s.pending |= Bit(signum);
        siginfo_t& info = s.info[signum];
        info = {};
        info.si_signo = signum;
        info.si_code = code;
        info.si_pid = getpid();
        if (value != nullptr) info.si_value = *value;
      }
    } else {
      if (s.rt_queue_total >= SIGQUEUE_MAX) {
        return errno = EAGAIN, -1;
      }
      siginfo_t info = {};
      info.si_signo = signum;
      info.si_code = code;
      info.si_pid = getpid();
      if (value != nullptr) info.si_value = *value;
      s.rt_queues[signum].push_back(info);
      s.rt_queue_total++;
      s.pending |= Bit(signum);
    }
  }
  s.sent.notify_all();
  return 0;
}

// Takes the lowest pending signal in `set`, waiting until `deadline` (none:
// forever). Returns the signal, or -1 with errno EAGAIN when it times out.
int Take(const sigset_t* set, siginfo_t* info,
         const std::chrono::steady_clock::time_point* deadline) {
  State& s = state();
  std::unique_lock<std::mutex> guard(s.lock);
  auto ready = [&] { return (s.pending & *set) != 0; };
  if (deadline == nullptr) {
    s.sent.wait(guard, ready);
  } else if (!s.sent.wait_until(guard, *deadline, ready)) {
    return errno = EAGAIN, -1;
  }
  for (int signum = 1; signum <= kMaxSignal; ++signum) {
    if ((s.pending & *set & Bit(signum)) != 0) {
      PopSignalLocked(s, signum, info);
      return signum;
    }
  }
  return errno = EAGAIN, -1;  // not reached: ready() held
}

}  // namespace

extern "C" {

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
  if (act != nullptr) s.actions[signum] = *act;
  return 0;
}

int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
  if (oldset != nullptr) *oldset = thread_mask;
  if (set == nullptr) return 0;
  switch (how) {
    case SIG_BLOCK: thread_mask |= *set; break;
    case SIG_UNBLOCK: thread_mask &= ~*set; break;
    case SIG_SETMASK: thread_mask = *set; break;
    default: return EINVAL;
  }
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
  std::lock_guard<std::mutex> guard(s.lock);
  *set = s.pending;
  return 0;
}

int kill(pid_t pid, int sig) { return Send(pid, sig, SI_USER, nullptr); }

// The thread is not singled out: a signal sent to one of this process's
// threads is pending for the process, which is what sigwait sees.
int tgkill(pid_t tgid, pid_t, int sig) { return Send(tgid, sig, SI_TKILL, nullptr); }

int gesher_pthread_kill(pthread_t thread, int sig) {
  // winpthreads checks that the thread exists (and handles signal 0).
  int rc = pthread_kill(thread, 0);
  if (rc != 0 || sig == 0) return rc;
  return Send(getpid(), sig, SI_TKILL, nullptr) == 0 ? 0 : errno;
}

int sigqueue(pid_t pid, int sig, const union sigval value) {
  return Send(pid, sig, SI_QUEUE, &value);
}

int sigwaitinfo(const sigset_t* set, siginfo_t* info) { return Take(set, info, nullptr); }

int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout) {
  if (timeout == nullptr) return Take(set, info, nullptr);
  if (timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) return errno = EINVAL, -1;
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

  sigset_t old_mask = thread_mask;
  sigset_t temp_mask = *mask & ~(Bit(SIGKILL) | Bit(SIGSTOP));
  thread_mask = temp_mask;

  sigset_t unmasked = ~temp_mask;

  State& s = state();
  std::unique_lock<std::mutex> guard(s.lock);

  siginfo_t delivered_info = {};
  struct sigaction delivered_act = {};
  int delivered_sig = 0;

  for (;;) {
    int candidate = 0;
    for (int signum = 1; signum <= kMaxSignal; ++signum) {
      if ((s.pending & unmasked & Bit(signum)) != 0) {
        candidate = signum;
        break;
      }
    }

    if (candidate != 0) {
      struct sigaction act = s.actions[candidate];
      // Check if signal action is to ignore
      if (act.sa_handler == SIG_IGN ||
          (act.sa_handler == SIG_DFL &&
           (candidate == SIGCHLD || candidate == SIGURG ||
            candidate == SIGWINCH || candidate == SIGCONT))) {
        // Discard ignored signal
        PopSignalLocked(s, candidate, nullptr);
        continue;
      }

      // Check if signal action is default terminate
      if (act.sa_handler == SIG_DFL) {
        PopSignalLocked(s, candidate, &delivered_info);
        guard.unlock();
        thread_mask = old_mask;
        ExitProcess(128 + static_cast<UINT>(candidate));
      }

      // Action is a user-defined handler
      PopSignalLocked(s, candidate, &delivered_info);
      delivered_act = act;
      delivered_sig = candidate;
      if ((delivered_act.sa_flags & SA_RESETHAND) != 0) {
        s.actions[candidate] = {};
      }
      break;
    }

    s.sent.wait(guard);
  }

  guard.unlock();

  sigset_t handler_mask = temp_mask | delivered_act.sa_mask;
  if ((delivered_act.sa_flags & SA_NODEFER) == 0) {
    handler_mask |= Bit(delivered_sig);
  }
  thread_mask = handler_mask;

  if ((delivered_act.sa_flags & SA_SIGINFO) != 0) {
    if (delivered_act.sa_sigaction != nullptr) {
      ucontext_t ucontext = {};
      delivered_act.sa_sigaction(delivered_sig, &delivered_info, &ucontext);
    }
  } else {
    if (delivered_act.sa_handler != nullptr &&
        delivered_act.sa_handler != SIG_DFL &&
        delivered_act.sa_handler != SIG_IGN) {
      delivered_act.sa_handler(delivered_sig);
    }
  }

  thread_mask = old_mask;
  return errno = EINTR, -1;
}

}  // extern "C"
