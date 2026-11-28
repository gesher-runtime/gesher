// gesher bionic shim: ptrace(2) between Gesher processes
// (docs/decisions/0093-cross-process-ptrace.md).
//
// A tracer and its tracee are separate Windows processes. What Linux keeps in
// the kernel for each process (dumpable, uid, Yama's ptracer exception, its
// tracer and whether it is stopped) is in gesher-base's per-process shared
// state (gesher/ptrace_state.h); this file is the tracer's side:
//  - PTRACE_ATTACH / PTRACE_SEIZE check Linux's permissions without
//    CAP_SYS_PTRACE and with Yama's ptrace_scope 1 (the same uid; the tracee
//    is dumpable; the tracer is its ancestor or the one PR_SET_PTRACER
//    allowed, or any with PR_SET_PTRACER_ANY; one tracer per tracee; never
//    the caller itself), then claim the tracee. ATTACH asks the tracee's
//    guard to stop it, as the attach SIGSTOP does: the guard suspends every
//    thread of the tracee at a safe point, and the tracer's wait4 then reports
//    the stop as Linux does (0x137f, WIFSTOPPED with SIGSTOP).
//  - PTRACE_INTERRUPT stops a seized tracee (PTRACE_EVENT_STOP).
//  - PTRACE_PEEKTEXT/PEEKDATA/POKETEXT/POKEDATA read and write the stopped
//    tracee's memory with ReadProcessMemory/WriteProcessMemory. A Gesher
//    process maps loaded code at the addresses the code sees, and the arm64
//    translator's guest addresses are host addresses (docs/arm64.md), so the
//    address a tracer passes is the tracee's address as is. A word that is
//    not mapped fails with EIO, as Linux's ptrace_access_vm. A POKE to a
//    mapped page that is not writable writes anyway, as Linux's FOLL_FORCE
//    does (copy-on-write for a file's pages), and tells the tracee's arm64
//    translator to drop code it translated.
//  - PTRACE_CONT and PTRACE_DETACH resume the tracee (DETACH also releases
//    it); a signal in `data` is then sent to it, except after an event stop.
//    A tracer that exits releases its tracees (kills them first with
//    PTRACE_O_EXITKILL); the tracee's guard watches it.
//  - PTRACE_TRACEME makes the parent the tracer, once.
// The registers of a stopped thread are not provided: an arm64 guest's live
// registers belong to the translator, not to the Windows thread context, so
// GETREGS/GETREGSET/PEEKUSER and the like fail with EIO (an unsupported
// request), as do requests Linux does not have. Signal-, syscall-, exec-,
// fork- and exit-stops are not produced either; options that would ask for
// them fail with EINVAL.
//
// Two entry points: `ptrace`, bionic's libc function (a PEEK returns the
// word), and the system call (a PEEK stores it at `data` and returns 0),
// which raw `syscall` instructions and syscall(2) reach.

#include "shim_internal.h"

#include <gesher/bionic_shim.h>
#include <gesher/fork.h>
#include <gesher/ptrace_state.h>

#include <windows.h>
#include <winternl.h>

#include <errno.h>
#include <unistd.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "exec_spawn.h"

namespace gesher::bionic {
namespace {

namespace pt = gesher::ptrace;

constexpr int kTraceme = 0, kPeektext = 1, kPeekdata = 2, kPoketext = 4,
              kPokedata = 5, kCont = 7, kKill = 8, kDetach = 17, kAttach = 16,
              kSetoptions = 0x4200, kSeize = 0x4206, kInterrupt = 0x4207;
constexpr int kEIO = 5;
constexpr uint64_t kOptTracesysgood = 1;
constexpr uint64_t kOptExitkill = static_cast<uint64_t>(pt::kOptExitkill);
constexpr uint64_t kSupportedOptions = kOptTracesysgood | kOptExitkill;
constexpr int kStatusAttachStop = 0x137f;  // WIFSTOPPED, WSTOPSIG SIGSTOP
constexpr int kStatusEventStop = 0x80057f;  // SIGTRAP | PTRACE_EVENT_STOP << 8
constexpr int kWnowait = 0x01000000, kWclone = static_cast<int>(0x80000000u),
              kWall = 0x40000000;
constexpr int kMaxSignal = 64;
// How long after a process's creation ATTACH waits for it to publish its
// ptrace state and start its guard (its executable's start-up, before main;
// a loaded machine can take seconds). A process older than that without a
// state is not a Gesher process: EPERM at once.
constexpr ULONGLONG kStartupWaitMs = 10000;

enum class TraceeState { kRunning, kStopPending, kStopped };

struct Tracee {
  HANDLE process = nullptr;
  pt::Remote remote;
  TraceeState state = TraceeState::kRunning;
  int64_t requested_epoch = 0;  // the guard's stop count when the stop was asked
  int stop_status = 0;
  bool seized = false;
};

std::mutex g_mutex;
std::map<int, Tracee> g_tracees;  // this process's tracees, by pid
DWORD g_tracees_pid = 0;          // the process they belong to (not a clone of it)

void Release(Tracee& t) {
  pt::CloseRemote(&t.remote);
  if (t.process != nullptr) CloseHandle(t.process);
  t.process = nullptr;
}

// The map, emptied in a process cloned from the tracer: Linux's forked child
// traces nothing. A fork empties it in the child at once (PtraceForkChild);
// in a clone made otherwise (none is) its views are unmapped here, and its
// handles, whose inheritance is not known, left alone. Called with g_mutex
// held.
std::map<int, Tracee>& Tracees() {
  const DWORD self = GetCurrentProcessId();
  if (g_tracees_pid != self) {
    if (g_tracees_pid != 0) {
      for (auto& entry : g_tracees) {
        if (entry.second.remote.state != nullptr) UnmapViewOfFile(entry.second.remote.state);
      }
    }
    g_tracees.clear();
    g_tracees_pid = self;
  }
  return g_tracees;
}

// --- ancestry (Yama) -----------------------------------------------------------

// The process that created `process`, while it runs and is older than it;
// 0 otherwise (Linux reparents an orphan to init).
DWORD ParentOf(HANDLE process) {
  struct BasicInformation {
    LONG exit_status;
    void* peb;
    ULONG_PTR affinity;
    LONG base_priority;
    ULONG_PTR pid;
    ULONG_PTR parent_pid;
  };
  using QueryFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
  BasicInformation info = {};
  if (query == nullptr || query(process, 0, &info, sizeof(info), nullptr) < 0) return 0;
  const DWORD parent = static_cast<DWORD>(info.parent_pid);
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
  if (h == nullptr) return 0;
  DWORD code = 0;
  const int64_t parent_start = pt::ProcessStartTime(h);
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && parent_start != 0 &&
                     parent_start <= pt::ProcessStartTime(process);
  CloseHandle(h);
  return alive ? parent : 0;
}

// Whether `ancestor` is `process` (when `or_self`) or one of its ancestors.
bool HasAncestor(HANDLE process, DWORD pid, DWORD ancestor, bool or_self) {
  if (or_self && pid == ancestor) return true;
  HANDLE current = process;
  bool owned = false;
  bool found = false;
  for (int depth = 0; depth < 64; ++depth) {
    const DWORD parent = ParentOf(current);
    if (owned) CloseHandle(current);
    owned = false;
    if (parent == 0) break;
    if (parent == ancestor) {
      found = true;
      break;
    }
    current = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    if (current == nullptr) break;
    owned = true;
  }
  if (owned) CloseHandle(current);
  return found;
}

// --- requests ------------------------------------------------------------------

int Fail(int error) {
  errno = error;
  return -1;
}

// PTRACE_ATTACH / PTRACE_SEIZE (kernel/ptrace.c, ptrace_attach).
int64_t Attach(int pid, bool seize, uint64_t addr, uint64_t options) {
  if (seize) {
    if (addr != 0) return Fail(kEIO);
    if ((options & ~kSupportedOptions) != 0) return Fail(kEINVAL);
  }
  if (pid <= 0) return Fail(kESRCH);
  const DWORD self = GetCurrentProcessId();
  constexpr DWORD kAccess = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_VM_READ |
                            PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
  HANDLE process = OpenProcess(kAccess, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    if (GetLastError() != ERROR_ACCESS_DENIED) return Fail(kESRCH);
    // A process this one may not open (another user's, a protected one).
    HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (q == nullptr) return Fail(kESRCH);
    CloseHandle(q);
    return Fail(kEPERM);
  }
  Tracee t;
  t.process = process;
  const int64_t start = pt::ProcessStartTime(process);
  if (static_cast<DWORD>(pid) != self) {
    // A process that has just started publishes its state and starts its
    // guard during its start-up: wait for them a bounded time. One that
    // publishes no state, or has no guard to stop it (gesher-base only in a
    // DLL), cannot be stopped as Linux stops a tracee.
    FILETIME now_time;
    GetSystemTimeAsFileTime(&now_time);
    const int64_t now = static_cast<int64_t>(
        (static_cast<uint64_t>(now_time.dwHighDateTime) << 32) | now_time.dwLowDateTime);
    const ULONGLONG age_ms = now > start ? static_cast<ULONGLONG>(now - start) / 10000 : 0;
    const ULONGLONG deadline =
        GetTickCount64() + (age_ms < kStartupWaitMs ? kStartupWaitMs - age_ms : 0);
    for (;;) {
      DWORD code = 0;
      if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE) {
        Release(t);
        return Fail(kESRCH);
      }
      // Only the state this very process published (a state an exited
      // process with the same pid left is not it).
      if (t.remote.state == nullptr) pt::OpenRemote(static_cast<DWORD>(pid), &t.remote, start);
      if (t.remote.state != nullptr && t.remote.state->guard_tid != 0) break;
      if (GetTickCount64() >= deadline) {
        Release(t);
        return Fail(kEPERM);
      }
      Sleep(5);
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& tracees = Tracees();
  if (static_cast<DWORD>(pid) != self && tracees.count(pid) != 0) {
    Release(t);
    return Fail(kEPERM);  // already ours
  }
  pt::SharedState* state = t.remote.state;
  pt::AccessFacts facts = {};
  facts.same_process = static_cast<DWORD>(pid) == self;
  if (!facts.same_process) {
    facts.tracer_uid = static_cast<int>(getuid());
    facts.tracee_uid = static_cast<int>(state->uid);
    facts.tracee_dumpable = static_cast<int>(state->dumpable);
    facts.tracee_ptracer = static_cast<int>(state->ptracer);
    facts.tracer_is_ancestor = HasAncestor(process, static_cast<DWORD>(pid), self, false);
    facts.tracer_descends_from_ptracer =
        facts.tracee_ptracer > 0 &&
        HasAncestor(GetCurrentProcess(), self, static_cast<DWORD>(facts.tracee_ptracer), true);
  }
  if (int error = pt::MayAttach(facts); error != 0) {
    Release(t);
    return Fail(-error);
  }
  // One tracer per tracee; a tracer that exited has released it. The tracer
  // and its creation time are claimed together.
  const int64_t me = pt::SelfTracerId();
  if (InterlockedCompareExchange64(&state->tracer, me, 0) != 0 &&
      (pt::LiveTracer(state) != 0 || InterlockedCompareExchange64(&state->tracer, me, 0) != 0)) {
    Release(t);
    return Fail(kEPERM);
  }
  InterlockedExchange(&state->options, seize ? static_cast<LONG>(options) : 0);
  t.seized = seize;
  if (!seize) {
    t.requested_epoch = state->stop_epoch;
    t.state = TraceeState::kStopPending;
    t.stop_status = kStatusAttachStop;
    InterlockedExchange(&state->want_stop, 1);
  }
  pt::Notify(t.remote);
  tracees.emplace(pid, t);
  return 0;
}

// PTRACE_TRACEME: the parent becomes the tracer (it is not stopped by this).
// Linux checks no uid or dumpable flag here (ptrace_traceme only asks the
// security modules, and Yama's scope 1 allows it).
int64_t Traceme() {
  pt::SharedState* state = pt::Self();
  if (state == nullptr) return Fail(kEPERM);
  const int parent = getppid();
  int64_t start = 0;
  if (parent != 1) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(parent));
    if (h != nullptr) {
      start = pt::ProcessStartTime(h);
      CloseHandle(h);
    }
  }
  const int64_t id = pt::TracerId(static_cast<DWORD>(parent), start);
  if (pt::LiveTracer(state) != 0 || InterlockedCompareExchange64(&state->tracer, id, 0) != 0) {
    return Fail(kEPERM);
  }
  // The guard watches the parent from now on.
  HANDLE request = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                              (L"Local\\gesher-ptrace-request-" +
                               std::to_wstring(GetCurrentProcessId())).c_str());
  if (request != nullptr) {
    SetEvent(request);
    CloseHandle(request);
  }
  return 0;
}

// The tracee `pid` of this process (kernel/ptrace.c, ptrace_check_attach):
// ESRCH unless this process traces it and, unless `any_state`, it is stopped.
// A child that called PTRACE_TRACEME naming this very process (its pid and
// creation time) is adopted here. Called with g_mutex.
Tracee* Find(int pid, bool any_state) {
  if (pid <= 0) return nullptr;
  auto& tracees = Tracees();
  auto it = tracees.find(pid);
  const int64_t me = pt::SelfTracerId();
  if (it == tracees.end()) {
    Tracee t;
    t.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_VM_READ |
                                PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                            FALSE, static_cast<DWORD>(pid));
    if (t.process == nullptr ||
        !pt::OpenRemote(static_cast<DWORD>(pid), &t.remote, pt::ProcessStartTime(t.process)) ||
        pt::LiveTracer(t.remote.state) != me) {
      Release(t);
      return nullptr;
    }
    it = tracees.emplace(pid, t).first;
    ChildWaitStateChanged();
  }
  Tracee& t = it->second;
  if (t.remote.state->tracer != me) return nullptr;  // released meanwhile
  if (any_state) return &t;
  const bool stopped = t.state != TraceeState::kRunning && t.remote.state->stopped != 0 &&
                       t.remote.state->stop_epoch > t.requested_epoch;
  if (!stopped) return nullptr;
  return &t;
}

// Asks the tracee's guard to resume it (the wait for that is WaitResumed,
// outside g_mutex).
void Resume(Tracee& t) {
  InterlockedExchange(&t.remote.state->want_stop, 0);
  pt::Notify(t.remote);
  t.state = TraceeState::kRunning;
}

// Waits until the guard has resumed `pid`, so that a stop asked right after is
// a new one: on the guard's answer, a bounded time.
void WaitResumed(int pid) {
  pt::Remote remote;
  if (!pt::OpenRemote(static_cast<DWORD>(pid), &remote)) return;
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  const ULONGLONG deadline = GetTickCount64() + 5000;
  while (remote.state->stopped != 0 && GetTickCount64() < deadline) {
    HANDLE handles[2] = {remote.done, process};
    const DWORD n = process != nullptr ? 2 : 1;
    if (remote.done == nullptr) {
      Sleep(1);
      continue;
    }
    if (WaitForMultipleObjects(n, handles, FALSE, 100) == WAIT_OBJECT_0 + 1) break;
  }
  if (process != nullptr) CloseHandle(process);
  pt::CloseRemote(&remote);
}

// POKE: writes a word as Linux's ptrace_access_vm with FOLL_FORCE does, also
// to a mapped page that is not writable (a breakpoint in code, a patched
// constant): such a page is made writable for the write (copy-on-write for
// a file's or an image's pages, so the file is not changed) and protected
// again. Returns false (EIO) for memory that is not mapped, or that cannot be
// written even so. `*code` is set when the page was not writable: code
// pages are not, for the arm64 translator, which must then retranslate.
bool ForceWrite(HANDLE process, uint64_t addr, uint64_t data, bool* code) {
  *code = false;
  SIZE_T n = 0;
  if (WriteProcessMemory(process, reinterpret_cast<void*>(addr), &data, sizeof(data), &n) &&
      n == sizeof(data)) {
    return true;
  }
  // Page by page: the word may straddle two pages.
  const uint64_t first = addr & ~uint64_t{0xfff};
  const uint64_t last = (addr + sizeof(data) - 1) & ~uint64_t{0xfff};
  struct Page {
    uint64_t base;
    DWORD old;
    bool changed;
  } pages[2] = {{first, 0, false}, {last, 0, false}};
  const int count = first == last ? 1 : 2;
  bool ok = true;
  for (int i = 0; i < count && ok; ++i) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQueryEx(process, reinterpret_cast<void*>(pages[i].base), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      ok = false;
      break;
    }
    const DWORD protect = mbi.Protect & 0xff;
    const bool exec = protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
                      protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    if (writable) continue;
    *code = true;
    const bool file = mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED;
    const DWORD wanted = file ? (exec ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY)
                              : (exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(pages[i].base), 0x1000, wanted,
                          &pages[i].old)) {
      ok = false;
      break;
    }
    pages[i].changed = true;
  }
  if (ok) {
    ok = WriteProcessMemory(process, reinterpret_cast<void*>(addr), &data, sizeof(data), &n) &&
         n == sizeof(data);
  }
  for (int i = 0; i < count; ++i) {
    if (!pages[i].changed) continue;
    // A copy-on-write page that was written is private now: protect it as
    // it was, without write access.
    DWORD old = 0;
    VirtualProtectEx(process, reinterpret_cast<void*>(pages[i].base), 0x1000, pages[i].old, &old);
  }
  return ok;
}

int64_t Request(int request, int pid, uint64_t addr, uint64_t data, bool syscall_abi) {
  switch (request) {
    case kTraceme:
      return Traceme();
    case kAttach:
    case kSeize: {
      const int64_t result = Attach(pid, request == kSeize, addr, data);
      if (result == 0) ChildWaitStateChanged();
      return result;
    }
    default:
      break;
  }
  int signal_to_send = 0;
  bool wait_resumed = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Tracee* t = Find(pid, request == kKill || request == kInterrupt);
    if (t == nullptr) return Fail(kESRCH);
    switch (request) {
      case kPeektext:
      case kPeekdata: {
        // ptrace_access_vm: a tracee that is no longer dumpable is not read.
        if (t->remote.state->dumpable == 0) return Fail(kEIO);
        uint64_t word = 0;
        SIZE_T n = 0;
        if (!ReadProcessMemory(t->process, reinterpret_cast<const void*>(addr), &word,
                               sizeof(word), &n) ||
            n != sizeof(word)) {
          return Fail(kEIO);
        }
        if (!syscall_abi) return static_cast<int64_t>(word);
        // The kernel stores the word at `data` (put_user: EFAULT).
        if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(data), &word,
                                sizeof(word), &n) ||
            n != sizeof(word)) {
          return Fail(kEFAULT);
        }
        return 0;
      }
      case kPoketext:
      case kPokedata: {
        if (t->remote.state->dumpable == 0) return Fail(kEIO);
        bool code = false;
        if (!ForceWrite(t->process, addr, data, &code)) return Fail(kEIO);
        FlushInstructionCache(t->process, reinterpret_cast<void*>(addr), sizeof(data));
        // POKETEXT and POKEDATA are the same on Linux: either may write code.
        // The tracee's translator drops what it translated from the word
        // before it runs on.
        pt::RecordCodeWrite(t->remote, addr, sizeof(data));
        return 0;
      }
      case kCont:
      case kDetach: {
        if (data > static_cast<uint64_t>(kMaxSignal)) return Fail(kEIO);
        // A signal is injected only at a signal-delivery stop (the attach
        // SIGSTOP); at an event stop Linux ignores it.
        signal_to_send = t->stop_status == kStatusEventStop ? 0 : static_cast<int>(data);
        Resume(*t);
        wait_resumed = true;
        if (request == kDetach) {
          InterlockedExchange(&t->remote.state->options, 0);
          InterlockedCompareExchange64(&t->remote.state->tracer, 0, pt::SelfTracerId());
          pt::Notify(t->remote);
          Release(*t);
          Tracees().erase(pid);
        }
        break;
      }
      case kInterrupt:
        if (!t->seized) return Fail(kEIO);
        if (t->state == TraceeState::kRunning) {
          t->requested_epoch = t->remote.state->stop_epoch;
          t->state = TraceeState::kStopPending;
          t->stop_status = kStatusEventStop;
          InterlockedExchange(&t->remote.state->want_stop, 1);
          pt::Notify(t->remote);
        }
        return 0;
      case kKill:
        signal_to_send = 9;
        break;
      case kSetoptions:
        if ((data & ~kSupportedOptions) != 0) return Fail(kEINVAL);
        InterlockedExchange(&t->remote.state->options, static_cast<LONG>(data));
        return 0;
      default:
        // Registers (GETREGS, GETREGSET, PEEKUSER...), single-stepping,
        // syscall stops, siginfo: not provided (see the top of this file).
        return Fail(kEIO);
    }
  }
  if (wait_resumed) WaitResumed(pid);
  if (signal_to_send != 0) KillOtherProcess(pid, signal_to_send);
  return 0;
}

// bionic's ptrace(): a PEEK returns the word.
int64_t BIONIC_SYSV Ptrace(int request, int pid, uint64_t addr, uint64_t data) {
  return Request(request, pid, addr, data, false);
}

// The system call (and bionic's __ptrace): a PEEK stores the word at `data`.
int64_t BIONIC_SYSV PtraceSyscall(int request, int pid, uint64_t addr, uint64_t data) {
  return Request(request, pid, addr, data, true);
}

// The arm64 translator's view of memory a tracer wrote (ptrace_state.h).
const pt::PublishedCodeWrites* BIONIC_SYSV PtraceCodeWrites() { return pt::CodeWrites(); }
void BIONIC_SYSV PtraceSetHaltHook(pt::HaltHook hook) { pt::SetHaltHook(hook); }

}  // namespace

void PtraceForkChild() {
  // The fork's clone inherited every handle at the same value
  // (gesher::CloneProcess) and copied the views: the child's copies of the
  // parent's tracees are released, and nothing in their states (the
  // parent's tracing) is touched. g_mutex is the fork's (the fork set).
  for (auto& entry : g_tracees) {
    pt::CloseRemote(&entry.second.remote);
    if (entry.second.process != nullptr) CloseHandle(entry.second.process);
  }
  g_tracees.clear();
  g_tracees_pid = GetCurrentProcessId();
}

int PtraceWaitPoll(int pid, int options, int* status, bool* matched, bool (*is_child)(int)) {
  *matched = false;
  if ((options & kWclone) != 0 && (options & kWall) == 0) return 0;  // tracees are not clones
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& tracees = Tracees();
  for (auto it = tracees.begin(); it != tracees.end(); ++it) {
    if (pid > 0 && it->first != pid) continue;
    Tracee& t = it->second;
    const int got = it->first;
    *matched = true;
    if (WaitForSingleObject(t.process, 0) == WAIT_OBJECT_0) {
      // Its exit: the parent's to report when this process is the parent.
      if (is_child != nullptr && is_child(got)) {
        Release(t);
        tracees.erase(it);
        return 0;
      }
      DWORD code = 0;
      GetExitCodeProcess(t.process, &code);
      if (status != nullptr) *status = gesher::exec::WaitStatus(code);
      if ((options & kWnowait) == 0) {
        Release(t);
        tracees.erase(it);
      }
      return got;
    }
    if (t.state == TraceeState::kStopPending && t.remote.state->stopped != 0 &&
        t.remote.state->stop_epoch > t.requested_epoch) {
      if (status != nullptr) *status = t.stop_status;
      if ((options & kWnowait) == 0) t.state = TraceeState::kStopped;
      return got;
    }
  }
  return 0;
}

void RegisterPtrace(SymbolTable& t) {
  t["ptrace"] = reinterpret_cast<void*>(&Ptrace);
  // The system call's ABI: raw `syscall`/`svc #0` and syscall(2) (x86-64 101,
  // arm64 117). Not a libc name.
  t["__gesher_ptrace_syscall"] = reinterpret_cast<void*>(&PtraceSyscall);
  t["__gesher_ptrace_code_writes"] = reinterpret_cast<void*>(&PtraceCodeWrites);
  t["__gesher_ptrace_set_halt_hook"] = reinterpret_cast<void*>(&PtraceSetHaltHook);
}

}  // namespace gesher::bionic

// fork (gesher/fork.h, ADR 0092): the tracee table in a forked child (which
// empties it: a forked child traces nothing, Tracees()).
GESHER_FORK_LOCK(gesher::kForkOrderProcess, std::mutex, gesher::bionic::g_mutex);
