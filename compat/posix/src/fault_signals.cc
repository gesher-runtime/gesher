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
// A vectored exception handler, first in the chain, does two things:
//
// 1. Stack growth. Windows commits a thread's stack page by page through a
//    guard page below the committed part; a read further down than the guard
//    page (a stack probe) is an access violation on reserved memory. When the
//    faulting address is in the current thread's stack reservation, above
//    its committed part, and reserved only, the pages down to it are
//    committed, a guard page is put below them, TEB.StackLimit follows, and
//    the instruction is retried. ART's protected pages at the bottom of the
//    stack are committed (no access) by mprotect (mman.cc), so a probe that
//    reaches them is not mistaken for growth.
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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <windows.h>

#include <pthread.h>

#include <initializer_list>

namespace {

constexpr uintptr_t kPage = 4096;
// The bottom of the stack left for ART's implicit stack overflow checks: its
// protected region (a page) plus the 32 KiB probe gap (ART_STACK_OVERFLOW_GAP_
// x86_64) plus a page of margin (ADR 0033).
constexpr uintptr_t kGrowthFloor = 64 * 1024;

thread_local bool t_unhandled = false;
thread_local int t_depth = 0;

// GESHER_TRACE_SIGNALS=1: the first 200 exceptions this handler sees, and what
// became of them, on stderr.
LONG g_traced = 0;
bool Tracing() {
  static const bool on = getenv("GESHER_TRACE_SIGNALS") != nullptr;
  return on;
}

void Trace(const EXCEPTION_RECORD* rec, const CONTEXT* c, const char* what) {
  if (!Tracing() || InterlockedIncrement(&g_traced) > 200) return;
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
}

void Unhandled(int, siginfo_t*, void*) { t_unhandled = true; }

bool GrowStack(uintptr_t addr) {
  NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  const uintptr_t limit = reinterpret_cast<uintptr_t>(tib->StackLimit);
  if (addr < low || addr >= limit) return false;
  // Leave the lowest part of the stack ungrown: ART installs its own protected
  // region there and relies on a fault for implicit stack overflow checks (its
  // compiled code and nterp probe the stack GAP bytes ahead, ADR 0033). Growing
  // it would defeat the check and let the stack run to the hard Windows limit
  // (STATUS_STACK_OVERFLOW). The margin covers ART's protected region and its
  // reserved gap; a genuine access there faults and ART throws
  // StackOverflowError.
  if (addr - low < kGrowthFloor) return false;
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
