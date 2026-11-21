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
//  - Ordinary kill() of another process uses gesher-base's existing mailbox.
//    Remote RT kill and queued-value sends return ENOSYS before that bitmap
//    transport. Ordinary external signals and SIGCHLD are pending if every
//    thread blocks them, to a thread waiting in sigsuspend/pause/a sleep (which
//    then returns EINTR), else on the shim's signal thread at once.
//  - Terminating defaults still end the process (abort() for the core defaults;
//    no core-file claim). Local default-stop delivery returns ENOSYS rather
//    than becoming termination. SIGCHLD, SIGCONT, SIGURG and SIGWINCH ignore.
//  - abort() runs a SIGABRT handler first, as bionic's does.
//  - Not done: synchronous signals from faults (SIGSEGV, SIGBUS, SIGFPE,
//    SIGILL, SIGTRAP) are not delivered to the .so's handlers: Windows
//    exceptions in loaded code go to ART's handling and the loader's
//    diagnostics; alarm/setitimer timers. An active SA_ONSTACK delivery returns
//    ENOSYS without consuming a pending record; synchronous sigwait remains
//    available regardless of that unused flag. SA_SIGINFO handlers get the
//    record's siginfo and a ucontext with the thread's actual registers at the
//    delivery point (BionicUcontext below); for an ARM64 handler run by the
//    x86-64 translator, whose guest registers the shim cannot report, the
//    delivery returns ENOSYS like SA_ONSTACK.
//  - SIGRTMIN is bionic's: 32 + __SIGRT_RESERVED (10) = 42; SIGRTMAX is 64.

#include "shim_internal.h"

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>
#include <gesher/pending_signals.h>
#include <unistd.h>
#include <gesher/clocks.h>
#include <gesher/resume.h>

#include <windows.h>

#include <errno.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

// gesher-base's mailbox hook (compat/posix/src/process.cc): signals from
// other processes go to it first.
extern "C" int (*gesher_signal_hook)(int sig, int sender_pid);

namespace gesher::bionic {

namespace {

constexpr int kNsig = 65;  // signals 1..64
// Android 16 Bionic reserves kernel realtime signals 32..41 for its runtime.
// Public SIGRTMIN/SIGRTMAX call these exports rather than expose kernel bounds.
constexpr int kSigrtMin = 32 + 10;
constexpr uintptr_t kSigDfl = 0, kSigIgn = 1;
constexpr int kSaSiginfo = 4, kSaOnstack = 0x08000000, kSaNodefer = 0x40000000;
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

static_assert(sizeof(BionicStack) == 24);

// Bionic's ucontext_t for the guest ABI (NDK sys/ucontext.h). The shim delivers
// a handler on the receiving thread itself, at the point where that thread
// takes the signal (raise, an unblocking mask change, sigsuspend, an
// interrupted wait). The machine context is that thread's actual register
// state there (RtlCaptureContext), as Linux reports the state at the kernel
// entry the signal interrupted; uc_sigmask is the mask the thread returns to.
// Changes a handler makes to the context are not applied on return.
#if defined(__x86_64__)
struct BionicUcontext {
  uint64_t uc_flags;
  void* uc_link;
  BionicStack uc_stack;
  int64_t gregs[23];
  void* fpregs;
  uint64_t reserved1[8];
  uint64_t uc_sigmask;
  uint8_t fpregs_mem[512];
};
static_assert(offsetof(BionicUcontext, gregs) == 40);
static_assert(offsetof(BionicUcontext, uc_sigmask) == 296);
static_assert(offsetof(BionicUcontext, fpregs_mem) == 304);
static_assert(sizeof(BionicUcontext) == 816);

void FillMachineContext(const CONTEXT& c, BionicUcontext* uc) {
  enum { R8, R9, R10, R11, R12, R13, R14, R15, RDI, RSI, RBP, RBX, RDX, RAX, RCX, RSP, RIP, EFL,
         CSGSFS };
  int64_t* g = uc->gregs;
  g[R8] = c.R8; g[R9] = c.R9; g[R10] = c.R10; g[R11] = c.R11;
  g[R12] = c.R12; g[R13] = c.R13; g[R14] = c.R14; g[R15] = c.R15;
  g[RDI] = c.Rdi; g[RSI] = c.Rsi; g[RBP] = c.Rbp; g[RBX] = c.Rbx;
  g[RDX] = c.Rdx; g[RAX] = c.Rax; g[RCX] = c.Rcx; g[RSP] = c.Rsp;
  g[RIP] = c.Rip;
  g[EFL] = c.EFlags;
  g[CSGSFS] = static_cast<int64_t>(c.SegCs) | (static_cast<int64_t>(c.SegGs) << 16) |
              (static_cast<int64_t>(c.SegFs) << 32);
  // CONTEXT.FltSave is an FXSAVE image, as _libc_fpstate is.
  static_assert(sizeof(uc->fpregs_mem) == sizeof(c.FltSave));
  memcpy(uc->fpregs_mem, &c.FltSave, sizeof(uc->fpregs_mem));
  uc->fpregs = uc->fpregs_mem;
}
#elif defined(__aarch64__)
struct alignas(16) BionicUcontext {
  uint64_t uc_flags;
  void* uc_link;
  BionicStack uc_stack;
  uint64_t uc_sigmask;
  uint8_t padding[120];
  alignas(16) uint64_t fault_address;
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  alignas(16) uint8_t reserved[4096];
};
static_assert(offsetof(BionicUcontext, uc_sigmask) == 40);
static_assert(offsetof(BionicUcontext, fault_address) == 176);
static_assert(offsetof(BionicUcontext, reserved) == 464);
static_assert(sizeof(BionicUcontext) == 4560);

void FillMachineContext(const CONTEXT& c, BionicUcontext* uc) {
  for (int i = 0; i < 29; ++i) uc->regs[i] = c.X[i];
  uc->regs[29] = c.Fp;
  uc->regs[30] = c.Lr;
  uc->sp = c.Sp;
  uc->pc = c.Pc;
  uc->pstate = c.Cpsr;
  // The kernel's FPSIMD record (asm/sigcontext.h fpsimd_context), then the
  // zero terminator already in place.
  struct Fpsimd {
    uint32_t magic, size, fpsr, fpcr;
    uint64_t vregs[64];
  };
  static_assert(sizeof(Fpsimd) == 528);
  Fpsimd fp{0x46508001u, sizeof(Fpsimd), c.Fpsr, c.Fpcr, {}};
  static_assert(sizeof(fp.vregs) == sizeof(c.V));
  memcpy(fp.vregs, c.V, sizeof(fp.vregs));
  memcpy(uc->reserved, &fp, sizeof(fp));
}
#endif

uint64_t Bit(int sig) { return uint64_t{1} << (sig - 1); }
bool Valid(int sig) { return sig >= 1 && sig < kNsig; }

struct ThreadState {
  std::atomic<uint64_t> mask{0};
  gesher::signals::Pending<BionicSiginfo> pending;
  HANDLE lifetime = nullptr;
  HANDLE retirement_wait = nullptr;
  uint64_t id = 0;
  ~ThreadState() { if (lifetime != nullptr) CloseHandle(lifetime); }
  std::atomic<bool> any_pending{false};
  BionicStack alt_stack{nullptr, 2 /* SS_DISABLE */, 0};
  // Signals this thread takes while it waits (sigsuspend, pause, sigwait, a
  // sleep); 0 when it does not wait. Under State::mutex.
  uint64_t waiting_for = 0;
  // The shim's own signal thread, not one of the program's.
  bool internal = false;
};

struct State {
  std::mutex mutex;
  std::condition_variable sent;
  BionicSigaction actions[kNsig] = {};
  gesher::signals::Pending<BionicSiginfo> process_pending;
  std::unordered_map<uint64_t, ThreadState*> threads;
};

State& S() {
  static State* s = new State();
  return *s;
}

thread_local ThreadState* t_state = nullptr;

// Actual kernel-thread exit retires this exact identity without requiring a
// later unrelated signal operation. The one-shot callback owns final deletion;
// no observer can delete a state while its wait callback is queued.
void CALLBACK RetireThread(void* context, BOOLEAN) {
  auto* thread = static_cast<ThreadState*>(context);
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    auto found = S().threads.find(thread->id);
    if (found != S().threads.end() && found->second == thread) S().threads.erase(found);
  }
  // Nonblocking unregister is required inside this wait's own callback.
  // WT_EXECUTEONLYONCE guarantees no second callback can retain the context.
  UnregisterWaitEx(thread->retirement_wait, nullptr);
  delete thread;
}

// Called under S().mutex. That same mutex prevents an already-signaled
// lifetime from retiring its context before publication is complete.
ThreadState* CreateThreadState(uint64_t id, HANDLE lifetime) {
  auto* thread = new (std::nothrow) ThreadState();
  if (thread == nullptr) { CloseHandle(lifetime); return nullptr; }
  thread->id = id;
  thread->lifetime = lifetime;
  if (!RegisterWaitForSingleObject(&thread->retirement_wait, lifetime, RetireThread,
                                   thread, INFINITE, WT_EXECUTEONLYONCE)) {
    delete thread;
    return nullptr;
  }
  S().threads.emplace(id, thread);
  return thread;
}

ThreadState* Self() {
  if (t_state == nullptr) {
    uint64_t id = PthreadSelfId();
    std::lock_guard<std::mutex> lock(S().mutex);
    auto found = S().threads.find(id);
    if (found != S().threads.end()) {
      t_state = found->second;
    } else {
      HANDLE lifetime = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &lifetime,
                           THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) return nullptr;
      t_state = CreateThreadState(id, lifetime);
    }
  }
  return t_state;
}

void Pop(gesher::signals::Pending<BionicSiginfo>& queue, int sig, BionicSiginfo* info) {
  if (!queue.Pop(sig, info) && info != nullptr) info->signo = sig;
}

bool DefaultIgnores(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28; }
bool DefaultStops(int sig) { return sig == 19 || sig == 20 || sig == 21 || sig == 22; }
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

// Where a guest (arm64) thread was when it raised a fatal signal or called
// abort(): gesher_arm64.dll prints its registers and backtrace, as debuggerd
// would. Nothing on an x86-64 thread or without the DLL.
void PrintGuestState() {
  HMODULE dll = GetModuleHandleA("gesher_arm64.dll");
  if (dll == nullptr) return;
  auto print = reinterpret_cast<void (*)()>(
      reinterpret_cast<void*>(GetProcAddress(dll, "gesher_arm64_print_guest_state")));
  if (print != nullptr) print();
}

[[noreturn]] void Terminate(int sig) {
  std::fprintf(stderr, "gesher bionic: signal %d (SIG%s) with its default action: terminating\n",
               sig, sig < 32 && kNames[sig] ? kNames[sig] : "RT");
  if (DefaultDumpsCore(sig)) PrintGuestState();
  std::fflush(stdout);
  std::fflush(stderr);
  if (DefaultDumpsCore(sig)) std::abort();
  ExitProcess(128 + static_cast<UINT>(sig));
}

// Queueing and synchronous sigwait do not request a handler frame. Validate
// this boundary only when an action would actually run. A disabled alternate
// stack does not select an alternate-stack delivery path.
bool ActionSupported(ThreadState* self, int sig, const BionicSigaction& act) {
  if (act.handler == kSigIgn) return true;
  if (act.handler == kSigDfl) return !DefaultStops(sig);
#if defined(__x86_64__)
  // A translated ARM64 handler needs the guest's own registers in an ARM64
  // frame; the host CONTEXT is not that machine state.
  if ((act.sa_flags & kSaSiginfo) != 0 &&
      gesher::arm64::IsGuestCode(reinterpret_cast<const void*>(act.handler))) {
    return false;
  }
#endif
  return (act.sa_flags & kSaOnstack) == 0 || (self->alt_stack.ss_flags & 2) != 0;
}

// Claim the disposition together with the pending record under the state
// lock. A concurrent sigaction must not turn a successfully claimed ordinary
// handler into an unsupported frame after its record has been consumed.
bool ClaimActionLocked(ThreadState* self, int sig, BionicSigaction* act) {
  *act = S().actions[sig];
  if (!ActionSupported(self, sig, *act)) return false;
  if (act->handler != kSigDfl && act->handler != kSigIgn &&
      (static_cast<uint32_t>(act->sa_flags) & kSaResethand) != 0) {
    S().actions[sig] = {};
  }
  return true;
}

// `return_mask` is the mask the thread goes back to after the handler (for
// sigsuspend, the mask from before the call), reported as uc_sigmask.
void Dispatch(ThreadState* self, int sig, const BionicSiginfo& info, const BionicSigaction& act,
              const uint64_t* return_mask = nullptr) {
  if (act.handler == kSigIgn) return;
  if (act.handler == kSigDfl) {
    if (DefaultIgnores(sig)) return;
    Terminate(sig);
  }
  gesher::elf::SetupThreadTls();  // the handler is loaded code
  uint64_t saved = self->mask;
  if ((act.sa_flags & kSaSiginfo) != 0) {
    CONTEXT context;
    RtlCaptureContext(&context);
    BionicUcontext frame{};
    BionicUcontext* uc = &frame;
    {
      std::lock_guard<std::mutex> lock(S().mutex);
      uc->uc_stack = self->alt_stack;
    }
    uc->uc_sigmask = return_mask != nullptr ? *return_mask : saved;
    FillMachineContext(context, uc);
    BionicSiginfo copy = info;
    self->mask |= act.sa_mask | ((act.sa_flags & kSaNodefer) != 0 ? 0 : Bit(sig));
    reinterpret_cast<void(BIONIC_SYSV*)(int, BionicSiginfo*, void*)>(act.handler)(sig, &copy, uc);
  } else {
    self->mask |= act.sa_mask | ((act.sa_flags & kSaNodefer) != 0 ? 0 : Bit(sig));
    reinterpret_cast<void(BIONIC_SYSV*)(int)>(act.handler)(sig);
  }
  self->mask = saved;
}

int Run(ThreadState* self, int sig, const BionicSiginfo& info) {
  BionicSigaction act;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    if (!ClaimActionLocked(self, sig, &act)) return kENOSYS;
  }
  Dispatch(self, sig, info, act);
  return 0;
}

// Takes one pending signal this thread does not block (its own first, then
// the process's). Returns 0 if there is none.
int TakeDeliverable(ThreadState* self, BionicSiginfo* info, BionicSigaction* act) {
  std::lock_guard<std::mutex> lock(S().mutex);
  for (int sig = 1; sig < kNsig; ++sig) {
    if ((self->mask & Bit(sig)) != 0) continue;
    if ((self->pending.Mask() & Bit(sig)) != 0) {
      if (!ClaimActionLocked(self, sig, act)) return -kENOSYS;
      Pop(self->pending, sig, info);
      self->any_pending.store(self->pending.Mask() != 0, std::memory_order_relaxed);
      return sig;
    }
    if ((S().process_pending.Mask() & Bit(sig)) != 0) {
      if (!ClaimActionLocked(self, sig, act)) return -kENOSYS;
      Pop(S().process_pending, sig, info);
      return sig;
    }
  }
  self->any_pending.store(self->pending.Mask() != 0, std::memory_order_relaxed);
  return 0;
}

std::atomic<bool> g_process_pending{false};

int DeliverAll(ThreadState* self) {
  BionicSiginfo info;
  BionicSigaction act;
  int sig;
  while ((sig = TakeDeliverable(self, &info, &act)) > 0) Dispatch(self, sig, info, act);
  std::lock_guard<std::mutex> lock(S().mutex);
  g_process_pending.store(S().process_pending.Mask() != 0, std::memory_order_relaxed);
  return sig < 0 ? -sig : 0;
}

BionicSiginfo MakeInfo(int sig, int code, uint64_t value) {
  BionicSiginfo info{};
  info.signo = sig;
  info.code = code;
  info.pid = static_cast<int32_t>(GetCurrentProcessId());
  info.uid = static_cast<uint32_t>(getuid());
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
  if (self == nullptr) return errno = kEAGAIN, -1;
  BionicSiginfo info = MakeInfo(sig, code, value);
  if ((self->mask & Bit(sig)) != 0) {
    std::lock_guard<std::mutex> lock(S().mutex);
    auto& queue = to_process ? S().process_pending : self->pending;
    if (!queue.Push(sig, code, info)) return errno = kEAGAIN, -1;
    if (to_process) g_process_pending.store(true, std::memory_order_release);
    else self->any_pending.store(true, std::memory_order_release);
    S().sent.notify_all();
    return 0;
  }
  int error = Run(self, sig, info);
  if (error != 0) return errno = error, -1;
  DeliverAll(self);
  return 0;
}

// --- the entry points ----------------------------------------------------------

int BIONIC_SYSV CurrentSigrtmin() { return kSigrtMin; }
int BIONIC_SYSV CurrentSigrtmax() { return kNsig - 1; }

int BIONIC_SYSV Sigaction(int sig, const BionicSigaction* act, BionicSigaction* old) {
  if (!Valid(sig) || (act != nullptr && (sig == kSigkill || sig == kSigstop))) {
    return errno = kEINVAL, -1;
  }
  std::lock_guard<std::mutex> lock(S().mutex);
  if (old != nullptr) *old = S().actions[sig];
  if (act != nullptr) {
    S().actions[sig] = *act;
    S().actions[sig].sa_mask &= ~(Bit(kSigkill) | Bit(kSigstop));
    if (act->handler == kSigIgn || (act->handler == kSigDfl && DefaultIgnores(sig))) {
      S().process_pending.Discard(sig);
      for (auto& entry : S().threads) {
        entry.second->pending.Discard(sig);
        entry.second->any_pending.store(entry.second->pending.Mask() != 0,
                                       std::memory_order_relaxed);
      }
      g_process_pending.store(S().process_pending.Mask() != 0, std::memory_order_relaxed);
    }
  }
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
  if (self == nullptr) return kEAGAIN;
  uint64_t current = self->mask;
  if (set == nullptr) {
    if (old != nullptr) *old = current;
    return 0;
  }
  if (set != nullptr) {
    if (how != kSigBlock && how != kSigUnblock && how != kSigSetmask) return kEINVAL;
    // Match Android 16 Bionic's filter_reserved_signals on the REQUEST,
    // before applying the operation. Filtering the resulting mask instead
    // would change pre-existing reserved bits even for SIG_BLOCK/UNBLOCK.
    // Signal 32 belongs to POSIX timers; runtime signals 33..41 must not be
    // newly blocked through the public mask functions.
    uint64_t requested = *set;
    if (how == kSigUnblock) {
      requested &= ~Bit(32);
      for (int sig = 33; sig < kSigrtMin; ++sig) requested |= Bit(sig);
    } else {
      requested |= Bit(32);
      for (int sig = 33; sig < kSigrtMin; ++sig) requested &= ~Bit(sig);
    }
    uint64_t next;
    switch (how) {
      case kSigBlock: next = current | requested; break;
      case kSigUnblock: next = current & ~requested; break;
      case kSigSetmask: next = requested; break;
      default: return kEINVAL;
    }
    // Refuse an unsupported delivery before changing the requested mask or
    // consuming a record. The blocked record remains available to sigwait.
    next &= ~(Bit(kSigkill) | Bit(kSigstop));
    std::lock_guard<std::mutex> lock(S().mutex);
    uint64_t deliverable = (self->pending.Mask() | S().process_pending.Mask()) & ~next;
    for (int sig = 1; sig < kNsig; ++sig) {
      if ((deliverable & Bit(sig)) != 0 && !ActionSupported(self, sig, S().actions[sig])) {
        return kENOSYS;
      }
    }
    self->mask = next;
  }
  if (old != nullptr) *old = current;
  int error = DeliverAll(self);
  if (error != 0) {
    self->mask = current;
    return error;
  }
  return 0;
}
int BIONIC_SYSV Sigprocmask(int how, const uint64_t* set, uint64_t* old) {
  int rc = PthreadSigmask(how, set, old);
  return rc == 0 ? 0 : (errno = rc, -1);
}

int BIONIC_SYSV Raise(int sig) { return SendFromSelf(sig, kSiTkill, 0, false); }

bool IsSelfPid(int pid) {
  return pid == 0 || pid == static_cast<int>(GetCurrentProcessId());
}

// Another process (a child, any Gesher process): process_bionic.cc, over
// gesher-base's signal mailboxes; the target's handlers run there.
int BIONIC_SYSV Kill(int pid, int sig) {
  if (sig != 0 && !Valid(sig)) return errno = kEINVAL, -1;
  // A broadcast/group request must not become a local RT send merely
  // because the legacy group-leader mapping resolves to this process.
  if (sig >= 32 && !IsSelfPid(pid)) return errno = kENOSYS, -1;
  if (pid == INT32_MIN) return errno = kESRCH, -1;
  int target = pid < -1 ? -pid : pid;  // a process group is its leader here
  if (!IsSelfPid(target)) {
    return KillOtherProcess(target, sig);
  }
  return SendFromSelf(sig, kSiUser, 0, true);
}
int BIONIC_SYSV Killpg(int pgrp, int sig) {
  if (pgrp < 0) return errno = kEINVAL, -1;
  return Kill(pgrp, sig);
}
int BIONIC_SYSV Sigqueue(int pid, int sig, uint64_t value) {
  if (sig != 0 && !Valid(sig)) return errno = kEINVAL, -1;
  if (!IsSelfPid(pid)) {
    if (sig == 0) return KillOtherProcess(pid, sig);
    return errno = kENOSYS, -1;
  }
  return SendFromSelf(sig, kSiQueue, value, true);
}

int BIONIC_SYSV PthreadKill(uint64_t thread, int sig) {
  if (sig != 0 && !Valid(sig)) return kEINVAL;
  if (thread == PthreadSelfId()) return SendFromSelf(sig, kSiTkill, 0, false) == 0 ? 0 : errno;
  void* lifetime = nullptr;
  int result = PthreadSignalTarget(thread, &lifetime);
  if (result != 0) return result;
  if (sig == 0) { CloseHandle(lifetime); return 0; }
  std::lock_guard<std::mutex> lock(S().mutex);
  if (WaitForSingleObject(lifetime, 0) != WAIT_TIMEOUT) {
    CloseHandle(lifetime);
    return kESRCH;
  }
  ThreadState* slot;
  auto found = S().threads.find(thread);
  if (found == S().threads.end()) {
    slot = CreateThreadState(thread, lifetime);
    if (slot == nullptr) return kEAGAIN;
  } else {
    CloseHandle(lifetime);
    slot = found->second;
  }
  const auto& action = S().actions[sig];
  if ((slot->mask & Bit(sig)) == 0 && !ActionSupported(slot, sig, action)) return kENOSYS;
  if ((slot->mask & Bit(sig)) == 0 &&
      (action.handler == kSigIgn || (action.handler == kSigDfl && DefaultIgnores(sig)))) return 0;
  if (!slot->pending.Push(sig, kSiTkill, MakeInfo(sig, kSiTkill, 0))) return kEAGAIN;
  slot->any_pending.store(true, std::memory_order_release);
  S().sent.notify_all();
  return 0;
}

int BIONIC_SYSV Sigpending(uint64_t* set) {
  ThreadState* self = Self();
  if (self == nullptr) return errno = kEAGAIN, -1;
  std::lock_guard<std::mutex> lock(S().mutex);
  *set = (self->pending.Mask() | S().process_pending.Mask()) & self->mask;
  return 0;
}

// Waits until a signal in `want` is pending for this thread or the process.
// Takes it and returns it, or 0 at the deadline.
int TakeWaiting(ThreadState* self, uint64_t want, BionicSiginfo* info,
                const std::chrono::steady_clock::time_point* deadline,
                BionicSigaction* act = nullptr) {
  std::unique_lock<std::mutex> lock(S().mutex);
  for (;;) {
    for (int sig = 1; sig < kNsig; ++sig) {
      if ((want & Bit(sig)) == 0) continue;
      if ((self->pending.Mask() & Bit(sig)) != 0) {
        if (act != nullptr && !ClaimActionLocked(self, sig, act)) {
          self->waiting_for = 0;
          return -kENOSYS;
        }
        Pop(self->pending, sig, info);
        self->any_pending.store(self->pending.Mask() != 0, std::memory_order_relaxed);
        self->waiting_for = 0;
        return sig;
      }
      if ((S().process_pending.Mask() & Bit(sig)) != 0) {
        if (act != nullptr && !ClaimActionLocked(self, sig, act)) {
          self->waiting_for = 0;
          return -kENOSYS;
        }
        Pop(S().process_pending, sig, info);
        self->waiting_for = 0;
        return sig;
      }
    }
    self->waiting_for = want;
    if (deadline == nullptr) {
      S().sent.wait(lock);
    } else if (S().sent.wait_until(lock, *deadline) == std::cv_status::timeout) {
      self->waiting_for = 0;
      return 0;
    }
  }
}

int BIONIC_SYSV Sigwait(const uint64_t* set, int* sig) {
  ThreadState* self = Self();
  if (self == nullptr) return kEAGAIN;
  int s = TakeWaiting(self, *set, nullptr, nullptr);
  *sig = s;
  return 0;
}
int BIONIC_SYSV Sigwaitinfo(const uint64_t* set, BionicSiginfo* info) {
  ThreadState* self = Self();
  if (self == nullptr) return errno = kEAGAIN, -1;
  return TakeWaiting(self, *set, info, nullptr);
}
int BIONIC_SYSV Sigtimedwait(const uint64_t* set, BionicSiginfo* info, const LinuxTimespec* ts) {
  if (ts == nullptr) return Sigwaitinfo(set, info);
  if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return errno = kEINVAL, -1;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ts->tv_sec) +
                  std::chrono::nanoseconds(ts->tv_nsec);
  ThreadState* self = Self();
  if (self == nullptr) return errno = kEAGAIN, -1;
  int sig = TakeWaiting(self, *set, info, &deadline);
  return sig != 0 ? sig : (errno = kEAGAIN, -1);
}

// sigsuspend: with `mask` in place, waits for a signal it lets through, runs
// it and returns -1/EINTR with the old mask back.
int BIONIC_SYSV Sigsuspend(const uint64_t* mask) {
  ThreadState* self = Self();
  if (self == nullptr) return errno = kEAGAIN, -1;
  uint64_t saved = self->mask;
  self->mask = *mask & ~(Bit(kSigkill) | Bit(kSigstop));
  BionicSiginfo info;
  for (;;) {
    BionicSigaction act;
    int sig = TakeWaiting(self, ~self->mask, &info, nullptr, &act);
    if (sig < 0) {
      self->mask = saved;
      return errno = -sig, -1;
    }
    if (act.handler == kSigIgn || (act.handler == kSigDfl && DefaultIgnores(sig))) continue;
    Dispatch(self, sig, info, act, &saved);
    break;
  }
  self->mask = saved;
  DeliverAll(self);
  return errno = kEINTR, -1;
}
int BIONIC_SYSV Pause() {
  ThreadState* self = Self();
  if (self == nullptr) return errno = kEAGAIN, -1;
  uint64_t mask = self->mask;
  return Sigsuspend(&mask);
}

int BIONIC_SYSV Sigaltstack(const BionicStack* ss, BionicStack* old) {
  ThreadState* self = Self();
  if (self == nullptr) return errno = kEAGAIN, -1;
  std::lock_guard<std::mutex> lock(S().mutex);
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
  if (self == nullptr) Terminate(kSigabrt);
  self->mask &= ~Bit(kSigabrt);
  uintptr_t handler;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    handler = S().actions[kSigabrt].handler;
  }
  if (handler != kSigDfl && handler != kSigIgn) Run(self, kSigabrt, MakeInfo(kSigabrt, kSiTkill, 0));
  Terminate(kSigabrt);
}

unsigned BIONIC_SYSV Alarm(unsigned) { return 0; }

// sys_siglist / sys_signame (bionic exports both, 65 entries).
const char* g_sys_siglist[kNsig];
const char* g_sys_signame[kNsig];

// --- signals from outside the program's threads ----------------------------------
//
// Another process's kill() reaches this one through gesher-base's mailbox
// (compat/posix/src/process.cc), on a thread-pool thread; a child's exit
// raises SIGCHLD from a wait callback. Linux delivers such a process-directed
// signal to any thread that does not block it. Here: if every program thread
// blocks it, it stays pending for the process (sigwait, a lowered mask take
// it); else a thread waiting for it (sigsuspend, pause, a sleep) takes it and
// returns EINTR; else the handler runs on the shim's signal thread, at once,
// even while the program's threads are busy.

std::atomic<bool> g_authoritative{false};
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
struct Queued {
  int sig;
  BionicSiginfo info;
};
std::vector<Queued>* g_queue = nullptr;  // under g_queue_mutex
bool g_signal_thread_started = false;

// Under g_queue_mutex. If a native worker cannot be created/registered, its
// already accepted ordinary external signals remain available at application
// delivery points instead of being stranded behind a nonexistent worker.
void RequeueExternalSignalsLocked() {
  std::lock_guard<std::mutex> lock(S().mutex);
  for (const auto& queued : *g_queue)
    S().process_pending.Push(queued.sig, queued.info.code, queued.info);
  g_queue->clear();
  g_signal_thread_started = false;
  g_process_pending.store(S().process_pending.Mask() != 0, std::memory_order_release);
  S().sent.notify_all();
}

DWORD WINAPI SignalThread(void*) {
  ThreadState* self = Self();
  if (self == nullptr) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    RequeueExternalSignalsLocked();
    return 1;
  }
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    self->internal = true;
  }
  for (;;) {
    Queued q;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return !g_queue->empty(); });
      q = g_queue->front();
      g_queue->erase(g_queue->begin());
    }
    self->mask = 0;
    if (Run(self, q.sig, q.info) != 0) {
      // A concurrent disposition change may now require an unsupported
      // frame. Preserve this external record for synchronous waiting.
      std::lock_guard<std::mutex> lock(S().mutex);
      S().process_pending.Push(q.sig, q.info.code, q.info);
      g_process_pending.store(true, std::memory_order_release);
      S().sent.notify_all();
    }
  }
}

void RunOnSignalThread(int sig, const BionicSiginfo& info) {
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  if (g_queue == nullptr) g_queue = new std::vector<Queued>();
  g_queue->push_back({sig, info});
  if (!g_signal_thread_started) {
    HANDLE t = CreateThread(nullptr, 1u << 20, SignalThread, nullptr, 0, nullptr);
    if (t == nullptr) {
      RequeueExternalSignalsLocked();
      return;
    }
    CloseHandle(t);
    g_signal_thread_started = true;
  }
  g_queue_cv.notify_all();
}

[[noreturn]] void TerminateBySignal(int sig) {
  std::fflush(stdout);
  std::fflush(stderr);
  ExitProcess(128 + static_cast<UINT>(sig));
}

// gesher-base's hook: called with every signal another process sends. C
// linkage, so its type matches the gesher_signal_hook pointer exactly.
extern "C" int GesherBaseHook(int sig, int sender) {
  return DeliverExternalSignal(sig, sender, kSiUser, 0, g_authoritative.load()) ? 1 : 0;
}

}  // namespace

bool DeliverExternalSignal(int sig, int sender_pid, int code, int status, bool owned) {
  if (!Valid(sig)) return false;
  BionicSigaction act;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    act = S().actions[sig];
  }
  if (act.handler == kSigDfl && !owned) return false;  // gesher-base's (ART's) signal
  if (act.handler == kSigIgn) return true;
  if (act.handler == kSigDfl) {
    // Stop signals have no effect here (SIGSTOP itself never comes this way).
    if (DefaultIgnores(sig) || sig == 20 || sig == 21 || sig == 22) return true;
    TerminateBySignal(sig);
  }
  BionicSiginfo info = MakeInfo(sig, code, static_cast<uint64_t>(static_cast<uint32_t>(status)));
  info.pid = sender_pid;
  {
    std::lock_guard<std::mutex> lock(S().mutex);
    bool any_thread = false, all_block = true, waiter = false;
    for (auto& entry : S().threads) {
      ThreadState* t = entry.second;
      if (t->internal) continue;
      any_thread = true;
      if ((t->mask & Bit(sig)) == 0) all_block = false;
      if ((t->waiting_for & Bit(sig)) != 0) waiter = true;
    }
    if ((any_thread && all_block) || waiter || (act.sa_flags & (kSaSiginfo | kSaOnstack)) != 0) {
      // This legacy external path supplies SI_USER / SIGCHLD. Actions that
      // require an application frame stay pending instead of receiving a
      // fabricated frame on the shim-owned signal thread.
      if (!S().process_pending.Push(sig, code, info)) return false;
      g_process_pending.store(true, std::memory_order_release);
      S().sent.notify_all();
      return true;
    }
  }
  RunOnSignalThread(sig, info);
  return true;
}

void InstallSignalHook(bool authoritative) {
  if (authoritative) g_authoritative.store(true);
  gesher_signal_hook = &GesherBaseHook;
}

namespace {
bool SleepWithObservation(uint32_t ms, uint32_t* left_ms,
                          void (*observe)(void*), void* context, int* error,
                          int64_t boottime_deadline = -1) {
  if (error != nullptr) *error = 0;
  ThreadState* self = Self();
  if (self == nullptr) {
    if (error != nullptr) *error = kEAGAIN;
    return false;
  }
  // Preserve the older wait entry's delivery behavior. The observed entry
  // must see an already-pending signal as an interruption too, before any
  // handler runs; otherwise nanosleep would silently consume it and sleep.
  if (observe == nullptr) {
    int result = DeliverAll(self);
    if (result != 0) {
      if (error != nullptr) *error = result;
      return false;
    }
  }
  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(ms);
  bool interrupted = false;
  int interrupted_signal = 0;
  BionicSiginfo interrupted_info{};
  BionicSigaction interrupted_action{};
  {
    std::unique_lock<std::mutex> lock(S().mutex);
    for (;;) {
      uint64_t deliverable = (self->pending.Mask() | S().process_pending.Mask()) & ~self->mask;
      if (observe != nullptr && deliverable != 0) {
        // Ignored signals do not interrupt nanosleep. Discard these pending
        // instances while holding the same lock as the action/queue state.
        for (int sig = 1; sig < kNsig; ++sig) {
          if ((deliverable & Bit(sig)) == 0) continue;
          uintptr_t handler = S().actions[sig].handler;
          if (handler == kSigIgn || (handler == kSigDfl && DefaultIgnores(sig))) {
            self->pending.Discard(sig);
            S().process_pending.Discard(sig);
            deliverable &= ~Bit(sig);
          }
        }
        self->any_pending.store(self->pending.Mask() != 0, std::memory_order_relaxed);
        g_process_pending.store(S().process_pending.Mask() != 0, std::memory_order_relaxed);
      }
      if (deliverable != 0) {
        if (observe != nullptr) {
          // Claim the waking signal before unlocking. Another waiting thread
          // may otherwise consume a process signal before our handler runs.
          for (int sig = 1; sig < kNsig; ++sig) {
            if ((deliverable & Bit(sig)) == 0) continue;
            if (!ClaimActionLocked(self, sig, &interrupted_action)) {
              self->waiting_for = 0;
              if (error != nullptr) *error = kENOSYS;
              return false;
            }
            interrupted_signal = sig;
            if ((self->pending.Mask() & Bit(sig)) != 0) {
              Pop(self->pending, sig, &interrupted_info);
            } else {
              Pop(S().process_pending, sig, &interrupted_info);
            }
            self->any_pending.store(self->pending.Mask() != 0, std::memory_order_relaxed);
            g_process_pending.store(S().process_pending.Mask() != 0, std::memory_order_relaxed);
            break;
          }
        }
        interrupted = true;
        break;
      }
      self->waiting_for = ~self->mask;
      if (boottime_deadline >= 0) {
        const int64_t now = gesher::BoottimeNanos();
        if (now >= boottime_deadline) break;
        const int64_t left = boottime_deadline - now;
        const int64_t millis = std::min<int64_t>(left / 1000000 + (left % 1000000 != 0),
                                               0x7ffffffe);
        // The resume callback takes this same mutex before notifying. A resume
        // between this clock check and wait cannot be lost; an earlier resume
        // is already reflected by the deadline predicate above.
        S().sent.wait_for(lock, std::chrono::milliseconds(millis));
      } else if (S().sent.wait_until(lock, deadline) == std::cv_status::timeout) {
        break;
      }
    }
    self->waiting_for = 0;
  }
  if (left_ms != nullptr) {
    auto now = std::chrono::steady_clock::now();
    *left_ms = now >= deadline ? 0
                               : static_cast<uint32_t>(
                                     std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
  }
  if (interrupted) {
    if (observe != nullptr) observe(context);
    if (interrupted_signal != 0) Dispatch(self, interrupted_signal, interrupted_info,
                                         interrupted_action);
    int result = DeliverAll(self);
    if (interrupted_signal == 0 && result != 0 && error != nullptr) *error = result;
  }
  return interrupted;
}
}  // namespace

bool InterruptibleSleep(uint32_t ms, uint32_t* left_ms, int* error = nullptr) {
  return SleepWithObservation(ms, left_ms, nullptr, nullptr, error);
}

bool InterruptibleSleepObserved(uint32_t ms, void (*observe)(void*), void* context,
                                int* error = nullptr) {
  return SleepWithObservation(ms, nullptr, observe, context, error);
}

int InterruptibleBoottimeSleepObserved(int64_t deadline, void (*observe)(void*), void* context) {
  gesher::ResumeSubscription resume;
  const int error = resume.Start([](void*) {
    std::lock_guard<std::mutex> lock(S().mutex);
    S().sent.notify_all();
  }, nullptr);
  if (error != 0) return error;
  // SleepWithObservation drops the signal-state lock before this subscription
  // is destroyed, so joining a resume callback cannot deadlock its notifier.
  int delivery_error = 0;
  bool interrupted = SleepWithObservation(0, nullptr, observe, context, &delivery_error, deadline);
  return delivery_error != 0 ? delivery_error : interrupted ? kEINTR : 0;
}

void DeliverPendingSignals() {
  ThreadState* self = t_state;
  if (self == nullptr) {
    if (!g_process_pending.load(std::memory_order_relaxed)) return;
    self = Self();
    if (self == nullptr) return;
  } else if (!self->any_pending.load(std::memory_order_acquire) &&
             !g_process_pending.load(std::memory_order_relaxed)) {
    return;
  }
  DeliverAll(self);
}

void RegisterSignal(SymbolTable& t) {
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("__libc_current_sigrtmin", CurrentSigrtmin);
  E("__libc_current_sigrtmax", CurrentSigrtmax);
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
