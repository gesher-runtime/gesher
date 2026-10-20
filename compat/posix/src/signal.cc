// gesher-base: Linux signal API on Windows.
//
// Status: signal sets, masks and handler registration work. Delivery of
// synchronous signals from Windows exceptions is the next step (needs the
// CONTEXT <-> ucontext_t translation from <sys/ucontext.h>).

#include <signal.h>

#include <errno.h>
#include <windows.h>

#include <atomic>
#include <mutex>

namespace {

constexpr int kMaxSignal = NSIG - 1;

struct State {
  std::mutex lock;
  struct sigaction actions[NSIG] = {};
};

State& state() {
  static State* s = new State();
  return *s;
}

thread_local sigset_t thread_mask = 0;
thread_local stack_t alt_stack = {nullptr, SS_DISABLE, 0};

bool Valid(int signum) { return signum >= 1 && signum <= kMaxSignal; }
sigset_t Bit(int signum) { return static_cast<sigset_t>(1) << (signum - 1); }

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
  *set = 0;
  return 0;
}

// Asynchronous signals between threads/processes are not delivered yet.
int sigwait(const sigset_t*, int*) { return ENOSYS; }
int sigsuspend(const sigset_t*) { return errno = ENOSYS, -1; }
int kill(pid_t, int) { return errno = ENOSYS, -1; }
int tgkill(pid_t, pid_t, int) { return errno = ENOSYS, -1; }
int sigqueue(pid_t, int, const union sigval) { return errno = ENOSYS, -1; }
int sigwaitinfo(const sigset_t*, siginfo_t*) { return errno = ENOSYS, -1; }
int sigtimedwait(const sigset_t*, siginfo_t*, const struct timespec*) { return errno = ENOSYS, -1; }

}  // extern "C"
