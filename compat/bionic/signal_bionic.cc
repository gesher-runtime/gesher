// gesher bionic shim: signals, as far as Windows allows.
//
// Windows has no signals. The shim keeps bionic's model for the .so's own
// use of them, entirely in user space and separate from gesher-base's and
// ART's signal handling (libsigchain): sigaction installs a System V handler
// in the shim's table; masks are per thread; raise, kill and pthread_kill
// generate signals, and handlers run on a thread that does not block them.
//
//  - raise(sig), kill(getpid(), sig) and pthread_kill(pthread_self(), sig) run
//    the handler on the calling thread before returning, as on Linux.
//  - A signal sent to another thread (pthread_kill), or to the process while
//    the caller blocks it, is pending: the target runs it at its next
//    delivery point, i.e. when it lowers its mask (sigprocmask,
//    pthread_sigmask, sigsuspend) or enters a sleep or poll through the shim
//    (nanosleep, usleep, sleep, poll, select, epoll_wait, pause). It cannot
//    interrupt a thread running or blocked elsewhere.
//  - sigwait/sigwaitinfo/sigtimedwait take pending signals.
//  - SIG_DFL for a signal whose default is to terminate ends the process
//    (abort() for the core-dumping ones); SIGCHLD, SIGCONT, SIGURG and
//    SIGWINCH are ignored.
//  - abort() runs a SIGABRT handler first, as bionic's does.
//  - Not done: synchronous signals from faults (SIGSEGV, SIGBUS, SIGFPE,
//    SIGILL, SIGTRAP) are not delivered to the .so's handlers: Windows
//    exceptions in loaded code go to ART's handling and the loader's
//    diagnostics; alarm/setitimer timers; sigaltstack is recorded only.

#include "shim_internal.h"

#include <gesher/elf_loader.h>

#include <windows.h>

#include <errno.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace gesher::bionic {

namespace {

constexpr int kNsig = 65;  // signals 1..64
constexpr uintptr_t kSigDfl = 0, kSigIgn = 1;
constexpr int kSaSiginfo = 4, kSaNodefer = 0x40000000;
constexpr uint32_t kSaResethand = 0x80000000u;
constexpr int kSigBlock = 0, kSigUnblock = 1, kSigSetmask = 2;
constexpr int kSigkill = 9, kSigstop = 19, kSigabrt = 6;
constexpr int kSiUser = 0, kSiQueue = -1, kSiTkill = -6;

// bionic's LP64 struct sigaction.
struct BionicSigaction {
  int32_t sa_flags;
  int32_t pad;
  uintptr_t handler;
  uint64_t sa_mask;
  uintptr_t sa_restorer;
};

// Linux x86-64 siginfo_t (128 bytes), the fields set here.
// (Field names without the si_ prefix: <signal.h> defines those as macros.)
struct BionicSiginfo {
  int32_t signo;
  int32_t error;
  int32_t code;
  int32_t pad;
  int32_t pid;
  uint32_t uid;
  uint64_t value;
  uint8_t rest[128 - 32];
};
static_assert(sizeof(BionicSiginfo) == 128);

struct BionicStack {
  void* ss_sp;
  int32_t ss_flags;
  uint64_t ss_size;
};

uint64_t Bit(int sig) { return uint64_t{1} << (sig - 1); }
bool Valid(int sig) { return sig >= 1 && sig < kNsig; }

struct ThreadState {
  uint64_t mask = 0;
  uint64_t pending = 0;
  BionicSiginfo info[kNsig] = {};
  std::atomic<bool> any_pending{false};
  BionicStack alt_stack{nullptr, 2 /* SS_DISABLE */, 0};
};

struct State {
  std::mutex mutex;
  std::condition_variable sent;
  BionicSigaction actions[kNsig] = {};
  uint64_t process_pending = 0;
  BionicSiginfo process_info[kNsig] = {};
  std::unordered_map<uint64_t, ThreadState*> threads;
};

State& S() {
  static State* s = new State();
  return *s;
}

thread_local ThreadState* t_state = nullptr;

// The calling thread's state; created on first use and kept (thread ids are
// not reused by the shim).
ThreadState* Self() {
  if (t_state == nullptr) {
    uint64_t id = PthreadSelfId();
    std::lock_guard<std::mutex> lock(S().mutex);
    auto& slot = S().threads[id];
    if (slot == nullptr) slot = new ThreadState();
    t_state = slot;
  }
  return t_state;
}

// Called with the lock held.
ThreadState* StateOf(uint64_t id) {
  auto& slot = S().threads[id];
  if (slot == nullptr) slot = new ThreadState();
  return slot;
}

bool DefaultIgnores(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28; }
bool DefaultDumpsCore(int sig) {
  return sig == 3 || sig == 4 || sig == 5 || sig == 6 || sig == 7 || sig == 8 || sig == 11 ||
         sig == 24 || sig == 25 || sig == 31;
}

const char* const kNames[kNsig] = {
    nullptr, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV",
    "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU",
    "URG", "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS"};
const char* const kDescriptions[kNsig] = {
    nullptr, "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap",
    "Aborted", "Bus error", "Floating point exception", "Killed", "User signal 1",
    "Segmentation fault", "User signal 2", "Broken pipe", "Alarm clock", "Terminated",
    "Stack fault", "Child exited", "Continue", "Stopped (signal)", "Stopped",
    "Stopped (tty input)", "Stopped (tty output)", "Urgent I/O condition", "CPU time limit exceeded",
    "File size limit exceeded", "Virtual timer expired", "Profiling timer expired",
    "Window size changed", "I/O possible", "Power failure", "Bad system call"};

[[noreturn]] void Terminate(int sig) {
  std::fprintf(stderr, "gesher bionic: signal %d (SIG%s) with its default action: terminating\n",
               sig, sig < 32 && kNames[sig] ? kNames[sig] : "RT");
  std::fflush(stdout);
  std::fflush(stderr);
  if (DefaultDumpsCore(sig)) std::abort();
  ExitProcess(128 + static_cast<UINT>(sig));
}

// Runs the action for `sig` on this thread, which does not block it.
void Run(ThreadState* self, int sig, const BionicSiginfo& info) {
  BionicSigaction act;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    act = S().actions[sig];
    if (act.handler != kSigDfl && act.handler != kSigIgn &&
        (static_cast<uint32_t>(act.sa_flags) & kSaResethand) != 0) {
      S().actions[sig] = {};
    }
  }
  if (act.handler == kSigIgn) return;
  if (act.handler == kSigDfl) {
    if (DefaultIgnores(sig)) return;
    Terminate(sig);
  }
  gesher::elf::SetupThreadTls();  // the handler is loaded code
  uint64_t saved = self->mask;
  self->mask |= act.sa_mask | ((act.sa_flags & kSaNodefer) != 0 ? 0 : Bit(sig));
  if ((act.sa_flags & kSaSiginfo) != 0) {
    BionicSiginfo copy = info;
    alignas(16) uint8_t ucontext[1024] = {};  // no machine context to report
    reinterpret_cast<void(BIONIC_SYSV*)(int, BionicSiginfo*, void*)>(act.handler)(sig, &copy,
                                                                                    ucontext);
  } else {
    reinterpret_cast<void(BIONIC_SYSV*)(int)>(act.handler)(sig);
  }
  self->mask = saved;
}

// Takes one pending signal this thread does not block (its own first, then
// the process's). Returns 0 if there is none.
int TakeDeliverable(ThreadState* self, BionicSiginfo* info) {
  std::lock_guard<std::mutex> lock(S().mutex);
  for (int sig = 1; sig < kNsig; ++sig) {
    if ((self->mask & Bit(sig)) != 0) continue;
    if ((self->pending & Bit(sig)) != 0) {
      self->pending &= ~Bit(sig);
      *info = self->info[sig];
      self->any_pending.store(self->pending != 0, std::memory_order_relaxed);
      return sig;
    }
    if ((S().process_pending & Bit(sig)) != 0) {
      S().process_pending &= ~Bit(sig);
      *info = S().process_info[sig];
      return sig;
    }
  }
  self->any_pending.store(self->pending != 0, std::memory_order_relaxed);
  return 0;
}

std::atomic<bool> g_process_pending{false};

void DeliverAll(ThreadState* self) {
  BionicSiginfo info;
  while (int sig = TakeDeliverable(self, &info)) Run(self, sig, info);
  std::lock_guard<std::mutex> lock(S().mutex);
  g_process_pending.store(S().process_pending != 0, std::memory_order_relaxed);
}

BionicSiginfo MakeInfo(int sig, int code, uint64_t value) {
  BionicSiginfo info{};
  info.signo = sig;
  info.code = code;
  info.pid = static_cast<int32_t>(GetCurrentProcessId());
  info.uid = 10000;
  info.value = value;
  return info;
}

// Generates `sig` from this thread, for itself (raise, pthread_kill of
// itself) or for the process (kill, sigqueue): runs it now unless the thread
// blocks it; then it is pending for the thread or the process, where another
// thread's sigwait or a lowered mask finds it.
int SendFromSelf(int sig, int code, uint64_t value, bool to_process) {
  if (sig == 0) return 0;
  if (!Valid(sig)) return errno = kEINVAL, -1;
  ThreadState* self = Self();
  BionicSiginfo info = MakeInfo(sig, code, value);
  if ((self->mask & Bit(sig)) != 0) {
    std::lock_guard<std::mutex> lock(S().mutex);
    if (to_process) {
      if ((S().process_pending & Bit(sig)) == 0) {
        S().process_pending |= Bit(sig);
        S().process_info[sig] = info;
      }
      g_process_pending.store(true, std::memory_order_release);
    } else {
      if ((self->pending & Bit(sig)) == 0) {
        self->pending |= Bit(sig);
        self->info[sig] = info;
      }
      self->any_pending.store(true, std::memory_order_release);
    }
    S().sent.notify_all();
    return 0;
  }
  Run(self, sig, info);
  DeliverAll(self);
  return 0;
}

// --- the entry points ----------------------------------------------------------

int BIONIC_SYSV Sigaction(int sig, const BionicSigaction* act, BionicSigaction* old) {
  if (!Valid(sig) || (act != nullptr && (sig == kSigkill || sig == kSigstop))) {
    return errno = kEINVAL, -1;
  }
  std::lock_guard<std::mutex> lock(S().mutex);
  if (old != nullptr) *old = S().actions[sig];
  if (act != nullptr) S().actions[sig] = *act;
  return 0;
}

uintptr_t BIONIC_SYSV Signal(int sig, uintptr_t handler) {
  BionicSigaction act{}, old{};
  act.handler = handler;
  act.sa_flags = 0x10000000;  // SA_RESTART
  if (Sigaction(sig, &act, &old) != 0) return ~uintptr_t{0};  // SIG_ERR
  return old.handler;
}

int BIONIC_SYSV Sigemptyset(uint64_t* set) {
  if (set == nullptr) return errno = kEINVAL, -1;
  *set = 0;
  return 0;
}
int BIONIC_SYSV Sigfillset(uint64_t* set) {
  if (set == nullptr) return errno = kEINVAL, -1;
  *set = ~uint64_t{0};
  return 0;
}
int BIONIC_SYSV Sigaddset(uint64_t* set, int sig) {
  if (set == nullptr || !Valid(sig)) return errno = kEINVAL, -1;
  *set |= Bit(sig);
  return 0;
}
int BIONIC_SYSV Sigdelset(uint64_t* set, int sig) {
  if (set == nullptr || !Valid(sig)) return errno = kEINVAL, -1;
  *set &= ~Bit(sig);
  return 0;
}
int BIONIC_SYSV Sigismember(const uint64_t* set, int sig) {
  if (set == nullptr || !Valid(sig)) return errno = kEINVAL, -1;
  return (*set & Bit(sig)) != 0 ? 1 : 0;
}

int BIONIC_SYSV PthreadSigmask(int how, const uint64_t* set, uint64_t* old) {
  ThreadState* self = Self();
  uint64_t current = self->mask;
  if (set != nullptr) {
    uint64_t next;
    switch (how) {
      case kSigBlock: next = current | *set; break;
      case kSigUnblock: next = current & ~*set; break;
      case kSigSetmask: next = *set; break;
      default: return kEINVAL;
    }
    // SIGKILL and SIGSTOP cannot be blocked.
    self->mask = next & ~(Bit(kSigkill) | Bit(kSigstop));
  }
  if (old != nullptr) *old = current;
  DeliverAll(self);
  return 0;
}
int BIONIC_SYSV Sigprocmask(int how, const uint64_t* set, uint64_t* old) {
  int rc = PthreadSigmask(how, set, old);
  return rc == 0 ? 0 : (errno = rc, -1);
}

int BIONIC_SYSV Raise(int sig) { return SendFromSelf(sig, kSiTkill, 0, false); }

bool IsSelfPid(int pid) {
  return pid == 0 || pid == -1 || pid == static_cast<int>(GetCurrentProcessId());
}

int BIONIC_SYSV Kill(int pid, int sig) {
  if (!IsSelfPid(pid)) return errno = kESRCH, -1;
  return SendFromSelf(sig, kSiUser, 0, true);
}
int BIONIC_SYSV Killpg(int pgrp, int sig) { return Kill(pgrp, sig); }
int BIONIC_SYSV Sigqueue(int pid, int sig, uint64_t value) {
  if (!IsSelfPid(pid)) return errno = kESRCH, -1;
  return SendFromSelf(sig, kSiQueue, value, true);
}

int BIONIC_SYSV PthreadKill(uint64_t thread, int sig) {
  if (sig != 0 && !Valid(sig)) return kEINVAL;
  if (thread == PthreadSelfId()) return SendFromSelf(sig, kSiTkill, 0, false) == 0 ? 0 : errno;
  if (sig == 0) return 0;
  std::lock_guard<std::mutex> lock(S().mutex);
  ThreadState* target = StateOf(thread);
  if ((target->pending & Bit(sig)) == 0) {
    target->pending |= Bit(sig);
    target->info[sig] = MakeInfo(sig, kSiTkill, 0);
  }
  target->any_pending.store(true, std::memory_order_release);
  S().sent.notify_all();
  return 0;
}

int BIONIC_SYSV Sigpending(uint64_t* set) {
  ThreadState* self = Self();
  std::lock_guard<std::mutex> lock(S().mutex);
  *set = (self->pending | S().process_pending) & self->mask;
  return 0;
}

// Waits until a signal in `want` is pending for this thread or the process.
// Takes it and returns it, or 0 at the deadline.
int TakeWaiting(ThreadState* self, uint64_t want, BionicSiginfo* info,
                const std::chrono::steady_clock::time_point* deadline) {
  std::unique_lock<std::mutex> lock(S().mutex);
  for (;;) {
    for (int sig = 1; sig < kNsig; ++sig) {
      if ((want & Bit(sig)) == 0) continue;
      if ((self->pending & Bit(sig)) != 0) {
        self->pending &= ~Bit(sig);
        if (info) *info = self->info[sig];
        self->any_pending.store(self->pending != 0, std::memory_order_relaxed);
        return sig;
      }
      if ((S().process_pending & Bit(sig)) != 0) {
        S().process_pending &= ~Bit(sig);
        if (info) *info = S().process_info[sig];
        return sig;
      }
    }
    if (deadline == nullptr) {
      S().sent.wait(lock);
    } else if (S().sent.wait_until(lock, *deadline) == std::cv_status::timeout) {
      return 0;
    }
  }
}

int BIONIC_SYSV Sigwait(const uint64_t* set, int* sig) {
  int s = TakeWaiting(Self(), *set, nullptr, nullptr);
  *sig = s;
  return 0;
}
int BIONIC_SYSV Sigwaitinfo(const uint64_t* set, BionicSiginfo* info) {
  return TakeWaiting(Self(), *set, info, nullptr);
}
int BIONIC_SYSV Sigtimedwait(const uint64_t* set, BionicSiginfo* info, const LinuxTimespec* ts) {
  if (ts == nullptr) return Sigwaitinfo(set, info);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ts->tv_sec) +
                  std::chrono::nanoseconds(ts->tv_nsec);
  int sig = TakeWaiting(Self(), *set, info, &deadline);
  return sig != 0 ? sig : (errno = kEAGAIN, -1);
}

// sigsuspend: with `mask` in place, waits for a signal it lets through, runs
// it and returns -1/EINTR with the old mask back.
int BIONIC_SYSV Sigsuspend(const uint64_t* mask) {
  ThreadState* self = Self();
  uint64_t saved = self->mask;
  self->mask = *mask & ~(Bit(kSigkill) | Bit(kSigstop));
  BionicSiginfo info;
  int sig = TakeWaiting(self, ~self->mask, &info, nullptr);
  Run(self, sig, info);
  self->mask = saved;
  DeliverAll(self);
  return errno = kEINTR, -1;
}
int BIONIC_SYSV Pause() {
  uint64_t mask = Self()->mask;
  return Sigsuspend(&mask);
}

int BIONIC_SYSV Sigaltstack(const BionicStack* ss, BionicStack* old) {
  ThreadState* self = Self();
  if (old != nullptr) *old = self->alt_stack;
  if (ss != nullptr) self->alt_stack = *ss;
  return 0;
}

const char* BIONIC_SYSV Strsignal(int sig) {
  if (sig >= 1 && sig < 32 && kDescriptions[sig] != nullptr) return kDescriptions[sig];
  static thread_local char buf[32];
  std::snprintf(buf, sizeof(buf), sig >= 34 && sig < kNsig ? "Real-time signal %d" : "Unknown signal %d",
                sig >= 34 ? sig - 34 : sig);
  return buf;
}
void BIONIC_SYSV Psignal(int sig, const char* s) {
  if (s != nullptr && *s != 0) {
    std::fprintf(stderr, "%s: %s\n", s, Strsignal(sig));
  } else {
    std::fprintf(stderr, "%s\n", Strsignal(sig));
  }
}

// abort(): bionic unblocks SIGABRT and raises it, so a handler the .so
// installed (a crash reporter) runs; if it returns, the process aborts.
[[noreturn]] void BIONIC_SYSV Abort() {
  ThreadState* self = Self();
  self->mask &= ~Bit(kSigabrt);
  uintptr_t handler;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    handler = S().actions[kSigabrt].handler;
  }
  if (handler != kSigDfl && handler != kSigIgn) Run(self, kSigabrt, MakeInfo(kSigabrt, kSiTkill, 0));
  std::fflush(stdout);
  std::fflush(stderr);
  std::abort();
}

unsigned BIONIC_SYSV Alarm(unsigned) { return 0; }

// sys_siglist / sys_signame (bionic exports both, 65 entries).
const char* g_sys_siglist[kNsig];
const char* g_sys_signame[kNsig];

}  // namespace

void DeliverPendingSignals() {
  ThreadState* self = t_state;
  if (self == nullptr) {
    if (!g_process_pending.load(std::memory_order_relaxed)) return;
    self = Self();
  } else if (!self->any_pending.load(std::memory_order_acquire) &&
             !g_process_pending.load(std::memory_order_relaxed)) {
    return;
  }
  DeliverAll(self);
}

void RegisterSignal(SymbolTable& t) {
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("sigaction", Sigaction);
  E("sigaction64", Sigaction);
  E("signal", Signal);
  E("bsd_signal", Signal);
  E("sysv_signal", Signal);
  E("sigemptyset", Sigemptyset);
  E("sigemptyset64", Sigemptyset);
  E("sigfillset", Sigfillset);
  E("sigfillset64", Sigfillset);
  E("sigaddset", Sigaddset);
  E("sigaddset64", Sigaddset);
  E("sigdelset", Sigdelset);
  E("sigdelset64", Sigdelset);
  E("sigismember", Sigismember);
  E("sigismember64", Sigismember);
  E("sigprocmask", Sigprocmask);
  E("sigprocmask64", Sigprocmask);
  E("pthread_sigmask", PthreadSigmask);
  E("pthread_sigmask64", PthreadSigmask);
  E("raise", Raise);
  E("kill", Kill);
  E("killpg", Killpg);
  E("sigqueue", Sigqueue);
  E("pthread_kill", PthreadKill);
  E("sigpending", Sigpending);
  E("sigpending64", Sigpending);
  E("sigwait", Sigwait);
  E("sigwait64", Sigwait);
  E("sigwaitinfo", Sigwaitinfo);
  E("sigwaitinfo64", Sigwaitinfo);
  E("sigtimedwait", Sigtimedwait);
  E("sigtimedwait64", Sigtimedwait);
  E("sigsuspend", Sigsuspend);
  E("sigsuspend64", Sigsuspend);
  E("pause", Pause);
  E("sigaltstack", Sigaltstack);
  E("strsignal", Strsignal);
  E("psignal", Psignal);
  E("abort", Abort);
  E("alarm", Alarm);
#undef E
  for (int i = 1; i < kNsig; ++i) {
    g_sys_siglist[i] = i < 32 ? kDescriptions[i] : "Real-time signal";
    g_sys_signame[i] = i < 32 ? kNames[i] : "RT";
  }
  t["sys_siglist"] = g_sys_siglist;
  t["sys_signame"] = g_sys_signame;
}

}  // namespace gesher::bionic
