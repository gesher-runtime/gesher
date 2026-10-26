// gesher-base: synchronous signals from Windows exceptions, and stack growth
// for stack probes.
//
// ART's compiled code and nterp rely on faults (docs/decisions/0033):
//   - implicit null checks: a load through a null reference faults, and
//     ART's SIGSEGV handler turns the fault into a NullPointerException;
//   - implicit stack overflow checks: each method's entry reads the stack
//     ART_STACK_OVERFLOW_GAP bytes below rsp (`testq %rax, -gap(%rsp)`); at
//     the bottom of the stack that read hits ART's protected pages, and the
//     handler throws StackOverflowError.
//
// A vectored exception handler, first in the chain, does three things:
//
// 1. Stack growth. Windows commits a thread's stack page by page through a
//    guard page below the committed part; a read further down than the guard
//    page (a stack probe) is an access violation on reserved memory. When the
//    faulting address is in the current thread's stack reservation, above
//    its committed part, and reserved only, the pages down to it are
//    committed, a guard page is put below them, TEB.StackLimit follows, and
//    the instruction is retried. ART's protected pages at the bottom of the
//    stack are committed (no access) by mprotect (mman.cc), so a probe that
//    reaches them is not mistaken for growth; nothing below them is grown.
//
// 2. Signals. EXCEPTION_ACCESS_VIOLATION becomes SIGSEGV (EXCEPTION_ILLEGAL_
//    INSTRUCTION SIGILL, integer divide by zero and overflow SIGFPE) for the
//    handler installed with sigaction (libsigchain's, in ART processes), with
//    a Linux siginfo_t and a ucontext_t made from the CONTEXT. Whatever the
//    handler changes in the ucontext goes back into the CONTEXT, and
//    execution continues there, as after sigreturn. The default action of
//    these signals is a gesher-base handler that only records that nobody
//    took the fault: the exception then goes on to Windows' own handling
//    (SEH, the crash reporter), so a first-chance fault in native code that
//    has a __try is not turned into a crash. (libsigchain forwards faults it
//    does not handle to the action it found when it claimed the signal.)
//
// 3. Stack overflow (docs/decisions/0036). Windows' own stack growth (a
//    guard page that moves down as it is touched) cannot be stopped above
//    ART's protected region: each guard page fault makes the next page down
//    a guard page, committed or not, and when the guard reaches the stack
//    guarantee at the bottom the kernel commits what is left and raises
//    EXCEPTION_STACK_OVERFLOW instead, on whatever touched it - for compiled
//    code and nterp, the stack probe. That exception is delivered as SIGSEGV
//    at its address, like the access violation it stands for, so ART's
//    handler throws StackOverflowError. ART's protected region sits right
//    above the guarantee (pthread_attr_getguardsize reports it), so the probe
//    that raises it is the one that would have hit the region; afterwards the
//    whole stack is committed, ART protects its region again, and the next
//    overflow on the thread is an access violation there. The handler runs
//    below the probing frame, ART_STACK_OVERFLOW_GAP (32 KiB) above the
//    region: no alternate stack is needed.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <windows.h>

#include <pthread.h>

#include <initializer_list>

#include "internal.h"

namespace {

constexpr uintptr_t kPage = 4096;

thread_local bool t_unhandled = false;
thread_local int t_depth = 0;

// GESHER_TRACE_SIGNALS=1: the first 200 exceptions this handler sees, and what
// became of them, on stderr.
LONG g_traced = 0;
LONG g_sentinel_traced = 0;
bool Tracing() {
  static const bool on = getenv("GESHER_TRACE_SIGNALS") != nullptr;
  return on;
}

void Trace(const EXCEPTION_RECORD* rec, const CONTEXT* c, const char* what, bool sentinel = false) {
  static const bool sentinel_on = getenv("GESHER_TRACE_INVALID_SENTINEL") != nullptr;
  if (!(Tracing() || (sentinel && sentinel_on)) ||
      InterlockedIncrement(sentinel ? &g_sentinel_traced : &g_traced) > (sentinel ? 8 : 200)) return;
  ULONG_PTR fault = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  uintptr_t base = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(c->Rip), &module) &&
      GetModuleFileNameA(module, name, sizeof(name)) != 0) {
    base = reinterpret_cast<uintptr_t>(module);
  }
  const char* file = strrchr(name, '\\');
  fprintf(stderr,
          "gesher-signal: thread %lu code 0x%08lx address 0x%llx rip %s+0x%llx (0x%llx) rsp 0x%llx: %s\n",
          GetCurrentThreadId(), rec->ExceptionCode, static_cast<unsigned long long>(fault),
          file != nullptr ? file + 1 : name, static_cast<unsigned long long>(c->Rip - base),
          static_cast<unsigned long long>(c->Rip), static_cast<unsigned long long>(c->Rsp), what);
  if (sentinel) {
    fprintf(stderr,
            "gesher-signal: rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rbp=%016llx\n"
            "gesher-signal: rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx r10=%016llx r11=%016llx\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9, c->R10, c->R11);
    fflush(stderr);
  }
}

void Unhandled(int, siginfo_t*, void*) { t_unhandled = true; }

bool GrowStack(uintptr_t addr) {
  NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  const uintptr_t limit = reinterpret_cast<uintptr_t>(tib->StackLimit);
  if (addr < low || addr >= limit) return false;
  // Leave the bottom of the stack ungrown: what Windows keeps (the stack
  // guarantee) and the page above it, where ART puts its protected region for
  // implicit stack overflow checks (its compiled code and nterp probe the
  // stack GAP bytes ahead, ADR 0033). A probe that skips over the region
  // faults there, and ART throws StackOverflowError.
  if (addr - low < gesher::StackBottomReserve() + kPage) return false;
  const uintptr_t page = addr & ~(kPage - 1);
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) == 0 ||
      mbi.State != MEM_RESERVE) {
    return false;  // committed: ART's protected pages, or not the stack's
  }
  // [page, limit): reserved pages and the old guard page just below limit.
  if (VirtualAlloc(reinterpret_cast<void*>(page), limit - page, MEM_COMMIT, PAGE_READWRITE) ==
      nullptr) {
    return false;
  }
  DWORD old;
  VirtualProtect(reinterpret_cast<void*>(page), limit - page, PAGE_READWRITE, &old);
  // A new guard page below, so that Windows keeps growing the stack as usual.
  if (page - kPage >= low &&
      VirtualQuery(reinterpret_cast<void*>(page - kPage), &mbi, sizeof(mbi)) != 0 &&
      mbi.State == MEM_RESERVE) {
    VirtualAlloc(reinterpret_cast<void*>(page - kPage), kPage, MEM_COMMIT,
                 PAGE_READWRITE | PAGE_GUARD);
  }
  tib->StackLimit = reinterpret_cast<void*>(page);
  return true;
}

void ToUcontext(const CONTEXT* c, const EXCEPTION_RECORD* rec, ucontext_t* uc) {
  greg_t* g = uc->uc_mcontext.gregs;
  g[REG_R8] = c->R8;
  g[REG_R9] = c->R9;
  g[REG_R10] = c->R10;
  g[REG_R11] = c->R11;
  g[REG_R12] = c->R12;
  g[REG_R13] = c->R13;
  g[REG_R14] = c->R14;
  g[REG_R15] = c->R15;
  g[REG_RDI] = c->Rdi;
  g[REG_RSI] = c->Rsi;
  g[REG_RBP] = c->Rbp;
  g[REG_RBX] = c->Rbx;
  g[REG_RDX] = c->Rdx;
  g[REG_RAX] = c->Rax;
  g[REG_RCX] = c->Rcx;
  g[REG_RSP] = c->Rsp;
  g[REG_RIP] = c->Rip;
  g[REG_EFL] = c->EFlags;
  g[REG_CSGSFS] = static_cast<greg_t>(c->SegCs) | (static_cast<greg_t>(c->SegGs) << 16) |
                  (static_cast<greg_t>(c->SegFs) << 32);
  g[REG_ERR] = 0;
  g[REG_TRAPNO] = rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? 14 : 0;
  g[REG_CR2] = rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2
                   ? static_cast<greg_t>(rec->ExceptionInformation[1])
                   : 0;
  // CONTEXT.FltSave is an FXSAVE image, as _libc_fpstate is.
  static_assert(sizeof(uc->__fpregs_mem) == sizeof(c->FltSave), "FXSAVE layout");
  memcpy(&uc->__fpregs_mem, &c->FltSave, sizeof(uc->__fpregs_mem));
  uc->uc_mcontext.fpregs = &uc->__fpregs_mem;
}

void FromUcontext(const ucontext_t* uc, CONTEXT* c) {
  const greg_t* g = uc->uc_mcontext.gregs;
  c->R8 = g[REG_R8];
  c->R9 = g[REG_R9];
  c->R10 = g[REG_R10];
  c->R11 = g[REG_R11];
  c->R12 = g[REG_R12];
  c->R13 = g[REG_R13];
  c->R14 = g[REG_R14];
  c->R15 = g[REG_R15];
  c->Rdi = g[REG_RDI];
  c->Rsi = g[REG_RSI];
  c->Rbp = g[REG_RBP];
  c->Rbx = g[REG_RBX];
  c->Rdx = g[REG_RDX];
  c->Rax = g[REG_RAX];
  c->Rcx = g[REG_RCX];
  c->Rsp = g[REG_RSP];
  c->Rip = g[REG_RIP];
  c->EFlags = static_cast<DWORD>(g[REG_EFL]);
  if (uc->uc_mcontext.fpregs != nullptr) {
    memcpy(&c->FltSave, uc->uc_mcontext.fpregs, sizeof(c->FltSave));
    c->MxCsr = uc->uc_mcontext.fpregs->mxcsr;
  }
}

LONG CALLBACK Dispatch(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  int signo;
  int code = 0;
  void* addr = rec->ExceptionAddress;
  switch (rec->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
      if (rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
      if (GrowStack(rec->ExceptionInformation[1])) {
        Trace(rec, info->ContextRecord, "stack grown");
        return EXCEPTION_CONTINUE_EXECUTION;
      }
      signo = SIGSEGV;
      addr = reinterpret_cast<void*>(rec->ExceptionInformation[1]);
      {
        MEMORY_BASIC_INFORMATION mbi;
        code = VirtualQuery(addr, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT
                   ? SEGV_ACCERR
                   : SEGV_MAPERR;
      }
      break;
    case EXCEPTION_STACK_OVERFLOW:
      // The last guard page at the bottom of the stack was touched (3. above):
      // an access violation at that address for ART's handler. Only for a
      // probe well below the stack pointer, as compiled code and nterp make:
      // native code that overflows (a push, a call) is left to Windows' own
      // handling and the crash report, as the few pages left are all the
      // stack there is to handle it on.
      if (rec->NumberParameters < 2 ||
          info->ContextRecord->Rsp - rec->ExceptionInformation[1] <= kPage ||
          info->ContextRecord->Rsp < rec->ExceptionInformation[1]) {
        return EXCEPTION_CONTINUE_SEARCH;
      }
      signo = SIGSEGV;
      code = SEGV_ACCERR;
      addr = reinterpret_cast<void*>(rec->ExceptionInformation[1]);
      break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
      signo = SIGILL;
      break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
      signo = SIGFPE;
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }

  struct sigaction act;
  if (gesher_sigaction_raw(signo, nullptr, &act) != 0) return EXCEPTION_CONTINUE_SEARCH;
  if ((act.sa_flags & SA_SIGINFO) == 0 &&
      (act.sa_handler == SIG_DFL || act.sa_handler == SIG_IGN)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if ((act.sa_flags & SA_SIGINFO) != 0 && act.sa_sigaction == Unhandled) {
    Trace(rec, info->ContextRecord, "no handler");
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // A fault in a handler that is handling a fault on this thread: once more
  // (libsigchain passes such a fault to the next handler), then give up.
  if (t_depth >= 2) return EXCEPTION_CONTINUE_SEARCH;

  siginfo_t si;
  memset(&si, 0, sizeof(si));
  si.si_signo = signo;
  si.si_code = code;
  si.si_addr = addr;
  ucontext_t uc;
  memset(&uc, 0, sizeof(uc));
  ToUcontext(info->ContextRecord, rec, &uc);
  pthread_sigmask(SIG_SETMASK, nullptr, &uc.uc_sigmask);
  sigset_t mask = uc.uc_sigmask | act.sa_mask;
  if ((act.sa_flags & SA_NODEFER) == 0) sigaddset(&mask, signo);
  pthread_sigmask(SIG_SETMASK, &mask, nullptr);

  const bool saved_unhandled = t_unhandled;
  t_unhandled = false;
  ++t_depth;
  // ART's fatal handler can exit without returning to our ordinary trace or
  // Windows' crash reporter. Capture this observed shutdown fault first; it
  // remains env-gated, separately bounded, and does not change disposition.
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      reinterpret_cast<uintptr_t>(addr) == ~uintptr_t{0}) {
    Trace(rec, info->ContextRecord, "invalid sentinel before signal handler", true);
  }
  if ((act.sa_flags & SA_SIGINFO) != 0) {
    act.sa_sigaction(signo, &si, &uc);
  } else {
    act.sa_handler(signo);
  }
  --t_depth;
  const bool unhandled = t_unhandled;
  t_unhandled = saved_unhandled;
  pthread_sigmask(SIG_SETMASK, &uc.uc_sigmask, nullptr);
  if (unhandled) {
    Trace(rec, info->ContextRecord, "not taken by the handler");
    return EXCEPTION_CONTINUE_SEARCH;
  }
  Trace(rec, info->ContextRecord, "taken by the handler");
  FromUcontext(&uc, info->ContextRecord);
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

namespace gesher {

size_t StackBottomReserve() {
  // The lowest page, then the guarantee: SetThreadStackGuarantee with 0 only
  // reads it. 0 means the default, two pages on x64 (measured: the guard
  // page that raises EXCEPTION_STACK_OVERFLOW is the fourth from the bottom).
  ULONG guarantee = 0;
  if (!SetThreadStackGuarantee(&guarantee)) guarantee = 0;
  uintptr_t reserve = (static_cast<uintptr_t>(guarantee) + kPage - 1) & ~(kPage - 1);
  if (reserve < 2 * kPage) reserve = 2 * kPage;
  return kPage + reserve;
}

}  // namespace gesher

extern "C" void gesher_install_fault_signals(void) {
  static bool installed = false;
  if (installed) return;
  installed = true;
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = Unhandled;
  act.sa_flags = SA_SIGINFO;
  for (int signo : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
    struct sigaction old;
    if (gesher_sigaction_raw(signo, nullptr, &old) == 0 && (old.sa_flags & SA_SIGINFO) == 0 &&
        old.sa_handler == SIG_DFL) {
      gesher_sigaction_raw(signo, &act, nullptr);
    }
  }
  AddVectoredExceptionHandler(1, Dispatch);
}
