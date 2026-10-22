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

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

// winpthreads' own, which gesher_pthread_kill calls; <pthread.h> routes
// everyone else to gesher_pthread_kill.
#undef pthread_kill

namespace {

constexpr int kMaxSignal = NSIG - 1;

struct State {
  std::mutex lock;
  struct sigaction actions[NSIG] = {};
  // Signals sent to the process and not taken yet; Linux keeps one of each
  // standard signal pending, as here.
  sigset_t pending = 0;
  siginfo_t info[NSIG] = {};
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

// Makes `signum` pending for the process. Signal 0 only checks the target.
int Send(pid_t pid, int signum, int code, const union sigval* value) {
  if (signum != 0 && !Valid(signum)) return errno = EINVAL, -1;
  if (pid != getpid() && pid != 0) {
    // Other processes would need a channel of their own.
    return errno = ENOSYS, -1;
  }
  if (signum == 0) return 0;
  State& s = state();
  {
    std::lock_guard<std::mutex> guard(s.lock);
    if ((s.pending & Bit(signum)) == 0) {
      s.pending |= Bit(signum);
      siginfo_t& info = s.info[signum];
      info = {};
      info.si_signo = signum;
      info.si_code = code;
      info.si_pid = getpid();
      if (value != nullptr) info.si_value = *value;
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
      s.pending &= ~Bit(signum);
      if (info != nullptr) *info = s.info[signum];
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

// Handlers are not run for asynchronous signals yet, so nothing would end
// the wait.
int sigsuspend(const sigset_t*) { return errno = ENOSYS, -1; }

}  // extern "C"
