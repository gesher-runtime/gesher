// gesher-base: what is left of Linux's process model on Windows (ADR 0045):
// the parent pid, signals to other processes, the uid, and resource limits.
//
// Signals between processes: every process linked with gesher-base keeps a
// mailbox, a named shared section `Local\gesher-signals-<pid>` with a pending
// mask, and a named event that a thread-pool wait watches. kill() on another
// pid sets the signal's bit and the event; the target then delivers it as a
// signal sent to itself (signal.cc: pending until sigwait, as ART's signal
// catcher takes SIGQUIT and SIGUSR1), except that one whose disposition is
// SIG_IGN, or SIG_DFL with a default action of ignoring it, is dropped, and
// SIGTERM, SIGINT and SIGHUP with SIG_DFL end it, as on Linux. Processes
// without a mailbox (not Gesher's) can be ended (signals whose default action
// ends a process) and stopped and continued (SIGSTOP, SIGCONT), not signalled
// otherwise. SIGKILL ends any process. A process ended by a signal exits with
// status 128 + the signal number.
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gesher/fork.h>
#include <gesher/ptrace_state.h>

#include "internal.h"

// Set by the bionic shim (compat/bionic/signal_bionic.cc) in processes that
// run loaded Android code: it gets each signal another process sends first
// and returns nonzero if it delivered it to the code's own handlers.
extern "C" {
int (*gesher_signal_hook)(int sig, int sender_pid) = nullptr;
int gesher_kill_other(pid_t pid, int sig);
}

namespace {

struct Mailbox {
  volatile LONG64 pending;  // bit (signal - 1)
  volatile LONG senders[NSIG];
  volatile LONG forked;     // a forked child's (gesher/fork.h): its exit codes
  // The process it belongs to (its creation time): a mailbox left by an
  // earlier process with the same pid (a sender still had it open) is reset
  // by the new owner rather than inherited with its pending signals.
  volatile LONG64 owner_created;
};

LONG64 CreationTime(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return static_cast<LONG64>((static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

// Makes `mailbox` the one of the process created at `created`: a stale
// mailbox (another owner) is emptied first.
void ClaimMailbox(Mailbox* mailbox, LONG64 created, bool forked) {
  if (InterlockedCompareExchange64(&mailbox->owner_created, 0, 0) != created) {
    InterlockedExchange64(&mailbox->pending, 0);
    InterlockedExchange(&mailbox->forked, 0);
    InterlockedExchange64(&mailbox->owner_created, created);
  }
  if (forked) InterlockedExchange(&mailbox->forked, 1);
}

std::wstring MailboxName(DWORD pid) { return L"Local\\gesher-signals-" + std::to_wstring(pid); }
std::wstring EventName(DWORD pid) { return L"Local\\gesher-signal-" + std::to_wstring(pid); }

Mailbox* g_mailbox = nullptr;

bool DefaultIgnores(int sig) {
  return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

// The program an exec in this process started, which replaces it: signals sent
// here go there (gesher::SetExecForward).
std::atomic<DWORD> g_exec_forward{0};

void Deliver(int sig) {
  if (const DWORD target = g_exec_forward.load(std::memory_order_acquire)) {
    gesher_kill_other(static_cast<pid_t>(target), sig);
    return;
  }
  // The bionic shim takes the signals its loaded code handles (exec2 track,
  // ADR 0055): gesher_signal_hook below.
  if (gesher_signal_hook != nullptr &&
      gesher_signal_hook(sig, static_cast<int>(g_mailbox->senders[sig])) != 0) {
    return;
  }
  struct sigaction sa = {};
  gesher_sigaction_raw(sig, nullptr, &sa);
  if (sa.sa_handler == SIG_IGN) return;
  if (sa.sa_handler == SIG_DFL) {
    if (DefaultIgnores(sig)) return;
    if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP) {
      TerminateProcess(GetCurrentProcess(), gesher::SignalExitCode(sig));
    }
  }
  kill(getpid(), sig);
}

void CALLBACK OnSignal(void*, BOOLEAN) {
  LONG64 bits = InterlockedExchange64(&g_mailbox->pending, 0);
  for (int sig = 1; sig < NSIG; ++sig) {
    if ((bits >> (sig - 1)) & 1) Deliver(sig);
  }
}

// A forked child's mailbox has no thread pool to watch its event (the
// parent's pool does not exist in the clone): a thread of its own does.
DWORD WINAPI MailboxThread(void* event) {
  for (;;) {
    if (WaitForSingleObject(static_cast<HANDLE>(event), INFINITE) != WAIT_OBJECT_0) return 1;
    std::lock_guard<std::mutex> helper(gesher::ForkHelperLock());
    OnSignal(nullptr, FALSE);
  }
}

extern const bool g_fork_exit_codes;

bool OpenMailbox(bool thread_pool) {
  DWORD pid = GetCurrentProcessId();
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Mailbox), MailboxName(pid).c_str());
  if (section == nullptr) return false;
  auto* mailbox = static_cast<Mailbox*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Mailbox)));
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, EventName(pid).c_str());
  if (mailbox == nullptr || event == nullptr) return false;
  // A forked child's mailbox already exists (its parent made it for this
  // process, and may have posted signals to it): those are delivered as soon
  // as it is watched.
  ClaimMailbox(mailbox, CreationTime(GetCurrentProcess()), !thread_pool || g_fork_exit_codes);
  g_mailbox = mailbox;
  if (thread_pool) {
    HANDLE wait = nullptr;
    return RegisterWaitForSingleObject(&wait, event, OnSignal, nullptr, INFINITE,
                                       WT_EXECUTEDEFAULT) != FALSE;
  }
  return gesher::StartHelperThread(MailboxThread, event, 64 * 1024);  // the section and event stay open for the life of the process
}

// A process whose parent uses the forked child's exit codes (a program a
// forked child exec'd, which replaces it, or spawned): the parent decodes this
// process's status with them, so its death by a signal uses them too
// (gesher/fork.h). The parent's mailbox says so; nothing is passed in the
// program's environment.
const bool g_fork_exit_codes = [] {
  const pid_t parent = getppid();
  return parent > 1 && gesher::IsForkedChild(static_cast<DWORD>(parent));
}();

// The mailbox of this process, made at startup (posix.cmake keeps this file
// linked into every executable).
const bool g_mailbox_ready = OpenMailbox(true);

using NtProcessFn = LONG(NTAPI*)(HANDLE);

NtProcessFn NtFunction(const char* name) {
  return reinterpret_cast<NtProcessFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)));
}

bool EndsByDefault(int sig) {
  return !DefaultIgnores(sig) && sig != SIGSTOP && sig != SIGTSTP;
}

}  // namespace

extern "C" {

// Anchors this file (-u in posix.cmake): the mailbox must exist in every process.
int gesher_signal_mailbox_anchor = 0;

// kill() of a pid other than this process's (signal.cc).
int gesher_kill_other(pid_t pid, int sig) {
  if (pid == -1) return errno = EPERM, -1;  // every process: not for an app
  if (pid < -1) pid = -pid;                 // a process group is one process
  if (pid == static_cast<pid_t>(GetCurrentProcessId())) return kill(pid, sig);
  DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE |
                 (sig == SIGKILL || EndsByDefault(sig) ? PROCESS_TERMINATE : 0) |
                 (sig == SIGSTOP || sig == SIGCONT ? PROCESS_SUSPEND_RESUME : 0);
  HANDLE process = OpenProcess(access, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    DWORD error = GetLastError();
    // A process that exists but is not ours (another user, a protected one).
    if (error == ERROR_ACCESS_DENIED) {
      HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
      if (q != nullptr) CloseHandle(q);
      return errno = q != nullptr ? EPERM : ESRCH, -1;
    }
    return errno = ESRCH, -1;
  }
  int result = 0;
  DWORD code = 0;
  if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE) {
    // Exited: Windows keeps the pid while handles to it are open, which says
    // nothing about whether its parent has waited for it; as after a wait.
    CloseHandle(process);
    return errno = ESRCH, -1;
  } else if (sig == 0) {
    // Existence and permission only.
  } else if (sig == SIGKILL) {
    const DWORD killed = gesher::IsForkedChild(static_cast<DWORD>(pid))
                             ? gesher::ForkSignalExitCode(SIGKILL)
                             : 128 + SIGKILL;
    if (!TerminateProcess(process, killed)) result = gesher::FailWithLastError();
  } else if (sig == SIGSTOP || sig == SIGCONT) {
    NtProcessFn fn = NtFunction(sig == SIGSTOP ? "NtSuspendProcess" : "NtResumeProcess");
    if (fn == nullptr || fn(process) < 0) result = (errno = EPERM, -1);
    if (sig == SIGCONT) {
      // A Gesher process also gets SIGCONT itself (its default: ignore).
    }
  }
  if (result == 0 && sig != 0 && sig != SIGKILL && sig != SIGSTOP &&
      !(GetExitCodeProcess(process, &code) && code != STILL_ACTIVE)) {
    HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, MailboxName(pid).c_str());
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(pid).c_str());
    Mailbox* mailbox = section != nullptr ? static_cast<Mailbox*>(MapViewOfFile(
                                                section, FILE_MAP_WRITE, 0, 0, sizeof(Mailbox)))
                                          : nullptr;
    // A mailbox some earlier process with this pid left is not the target's.
    const LONG64 owner = mailbox != nullptr ? InterlockedCompareExchange64(&mailbox->owner_created, 0, 0) : 0;
    if (mailbox != nullptr && owner != 0 && owner != CreationTime(process)) {
      UnmapViewOfFile(mailbox);
      mailbox = nullptr;
    }
    if (mailbox != nullptr && event != nullptr) {
      InterlockedExchange(&mailbox->senders[sig], static_cast<LONG>(GetCurrentProcessId()));
      InterlockedOr64(&mailbox->pending, static_cast<LONG64>(1) << (sig - 1));
      SetEvent(event);
    } else if (EndsByDefault(sig) && sig != SIGCONT) {
      // Not a Gesher process: the default action, which ends it.
      if (!TerminateProcess(process, 128 + sig)) result = gesher::FailWithLastError();
    }
    if (mailbox != nullptr) UnmapViewOfFile(mailbox);
    if (section != nullptr) CloseHandle(section);
    if (event != nullptr) CloseHandle(event);
  }
  CloseHandle(process);
  return result;
}

// The process that started this one, while it runs and is older than this
// one (Windows reuses pids); 1 otherwise, as Linux reparents an orphan to init.
pid_t getppid(void) {
  // NtQueryInformationProcess's ProcessBasicInformation (0).
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
  if (query == nullptr || query(GetCurrentProcess(), 0, &info, sizeof(info), nullptr) < 0) {
    return 1;
  }
  DWORD parent = static_cast<DWORD>(info.parent_pid);
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
  if (h == nullptr) return 1;
  FILETIME parent_created, own_created, exited, kernel, user;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE &&
               GetProcessTimes(h, &parent_created, &exited, &kernel, &user) &&
               GetProcessTimes(GetCurrentProcess(), &own_created, &exited, &kernel, &user) &&
               CompareFileTime(&parent_created, &own_created) <= 0;
  CloseHandle(h);
  return alive ? static_cast<pid_t>(parent) : 1;
}

// --- resource limits ---------------------------------------------------------------
//
// Limits Windows does not enforce are reported as Linux's defaults and can be
// lowered (and raised again up to the hard limit), as there; the fd limit is
// the CRT's (8192), which sysconf(_SC_OPEN_MAX) reports too.

namespace {

constexpr int kLimits = 16;  // RLIMIT_NLIMITS
std::mutex g_limits_mutex;
struct rlimit g_limits[kLimits];
bool g_limits_ready = false;

// The main thread's stack: the executable's stack reserve.
rlim_t MainStack() {
  auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return nt->OptionalHeader.SizeOfStackReserve;
}

void InitLimits() {
  if (g_limits_ready) return;
  for (struct rlimit& l : g_limits) l = {RLIM_INFINITY, RLIM_INFINITY};
  g_limits[RLIMIT_STACK] = {MainStack(), RLIM_INFINITY};
  g_limits[RLIMIT_CORE] = {0, RLIM_INFINITY};  // no core dumps
  g_limits[RLIMIT_NOFILE] = {8192, 8192};
  g_limits[RLIMIT_MEMLOCK] = {8 * 1024 * 1024, 8 * 1024 * 1024};
  g_limits[11] = {63438, 63438};          // RLIMIT_SIGPENDING
  g_limits[12] = {819200, 819200};        // RLIMIT_MSGQUEUE
  g_limits[13] = {0, 0};                  // RLIMIT_NICE
  g_limits[14] = {0, 0};                  // RLIMIT_RTPRIO
  g_limits_ready = true;
}

// Whether the caller's buffer can be written, as the kernel's copy_to_user
// decides: committed memory whose protection allows writing (a stack's guard
// page grows the stack). Linux answers EFAULT for any other buffer, and
// callers rely on it: Chromium's ProtectedMemory checks that a page is
// read-only by passing it to getrlimit and requiring EFAULT.
bool Writable(void* buffer, size_t size) {
  uintptr_t at = reinterpret_cast<uintptr_t>(buffer);
  if (at == 0 || size > UINTPTR_MAX - at) return false;
  const uintptr_t end = at + size;
  while (at < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT) {
      return false;
    }
    const DWORD protection = region.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
      return false;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  return true;
}

}  // namespace

int getrlimit(int resource, struct rlimit* rlim) {
  if (resource < 0 || resource >= kLimits) return errno = EINVAL, -1;
  if (!Writable(rlim, sizeof(*rlim))) return errno = EFAULT, -1;
  std::lock_guard<std::mutex> lock(g_limits_mutex);
  InitLimits();
  *rlim = g_limits[resource];
  return 0;
}

int setrlimit(int resource, const struct rlimit* rlim) {
  if (resource < 0 || resource >= kLimits) return errno = EINVAL, -1;
  if (rlim == nullptr) return errno = EFAULT, -1;
  if (rlim->rlim_cur > rlim->rlim_max) return errno = EINVAL, -1;
  std::lock_guard<std::mutex> lock(g_limits_mutex);
  InitLimits();
  // Raising a hard limit needs CAP_SYS_RESOURCE, which no process here has.
  if (rlim->rlim_max > g_limits[resource].rlim_max) return errno = EPERM, -1;
  g_limits[resource] = *rlim;
  return 0;
}

int prlimit(pid_t pid, int resource, const struct rlimit* new_limit, struct rlimit* old_limit) {
  if (pid != 0 && pid != static_cast<pid_t>(GetCurrentProcessId())) return errno = EPERM, -1;
  if (old_limit != nullptr && getrlimit(resource, old_limit) != 0) return -1;
  return new_limit != nullptr ? setrlimit(resource, new_limit) : 0;
}

int getrusage(int who, struct rusage* usage) {
  if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
    return errno = EINVAL, -1;
  }
  if (usage == nullptr) return errno = EFAULT, -1;
  memset(usage, 0, sizeof(*usage));
  if (who == RUSAGE_CHILDREN) return 0;  // children's times are not collected
  FILETIME create, exit, kernel, user;
  BOOL ok = who == RUSAGE_THREAD
                ? GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)
                : GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
  if (!ok) return gesher::FailWithLastError();
  auto to_timeval = [](FILETIME ft, struct timeval* tv) {
    unsigned long long t = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
                           ft.dwLowDateTime;  // 100 ns units
    tv->tv_sec = static_cast<long>(t / 10'000'000);
    tv->tv_usec = static_cast<long>((t % 10'000'000) / 10);
  };
  to_timeval(user, &usage->ru_utime);
  to_timeval(kernel, &usage->ru_stime);
  PROCESS_MEMORY_COUNTERS counters = {};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    usage->ru_maxrss = static_cast<long long>(counters.PeakWorkingSetSize / 1024);  // kB
    if (who == RUSAGE_SELF) usage->ru_minflt = counters.PageFaultCount;
  }
  return 0;
}

}  // extern "C"

// --- fork: a clone of this process (gesher/fork.h, ADR 0092) ----------------------

namespace {

std::atomic<bool> g_forked_child{false};

// ProcessHandleInformation (51): every handle with its object type.
struct HandleEntry {
  HANDLE value;
  ULONG_PTR handle_count;
  ULONG_PTR pointer_count;
  ACCESS_MASK granted_access;
  ULONG type_index;
  ULONG attributes;
  ULONG reserved;
};
struct HandleSnapshot {
  ULONG_PTR count;
  ULONG_PTR reserved;
  HandleEntry handles[1];
};
constexpr ULONG kObjInherit = 0x2;

using QueryProcessFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using QueryObjectFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
struct RtlProcessInformation {
  ULONG length;
  HANDLE process;
  HANDLE thread;
  CLIENT_ID client_id;
  BYTE image_information[64];  // SECTION_IMAGE_INFORMATION
};
using CloneFn = LONG(NTAPI*)(ULONG, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR, HANDLE,
                             RtlProcessInformation*);
constexpr ULONG kCloneInheritHandles = 0x2;  // RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES
constexpr LONG kStatusProcessCloned = 0x129;

void* Ntdll(const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
}

// --- the clone's address-wait table ----------------------------------------------
//
// ntdll's WaitOnAddress keeps its waiters in a hash table in the PEB,
// PEB.WaitOnAddressHashTable: 128 buckets, indexed by (address >> 5) & 0x7f,
// each the head of a list of wait blocks on the waiting threads' stacks, with
// two flag bits in its low bits. Every wait of a thread that goes through it
// uses it: WaitOnAddress itself, so Gesher's futexes and the guest's mutexes
// and condition variables, and ntdll's CRITICAL_SECTION waits.
// RtlCloneUserProcess copies the PEB with the rest of the address space, so
// the child's table still lists the parent's threads that were waiting at the
// clone, threads the child does not have. A wake in the child can go to one
// of those stale blocks and be lost: the child's real waiter then sleeps for
// ever. ntdll locks every heap for the clone, so a parent's thread that
// allocates is exactly one that waits then. The child's only thread waits for
// nothing at that point, so its table must be empty: it is emptied first
// thing, before any other thread exists in it.
//
// The field is in the public PEB symbols, at 0x3A0 in the 64-bit PEB (x64 and
// ARM64 use the same layout) since Windows 8; RtlpWaitOnAddress was checked
// to index it so on x64 build 26100. An offset that had moved would make the
// clear write over other fields, so it is used only when the layout checks
// out: Windows 8 or later (the real version, which compatibility modes do not
// change), and every non-empty bucket in the parent, just before the clone,
// pointing at committed private read-write memory (a thread's stack) on an
// 8-byte boundary. Otherwise fork fails with EAGAIN rather than make a child
// whose wakes may be lost. In the child only buckets that pass the same check
// are cleared. 32-bit builds have another layout: fork fails there.
//
// SRW locks and condition variables keep their waiters in the lock word
// itself (blocks on the waiters' stacks). The fork set makes the child's copy
// of each of its own locks anew; one outside it (a Windows DLL's, the app's)
// that a parent's thread waited on at the clone keeps that stale waiter in
// the child (ADR 0092).
#if defined(_WIN64)
struct PebLayout {
  BYTE before_wait_table[0x3A0];
  void* volatile wait_on_address_hash_table[128];  // PEB.WaitOnAddressHashTable
};
static_assert(offsetof(PebLayout, wait_on_address_hash_table) == 0x3A0,
              "PEB.WaitOnAddressHashTable is at 0x3A0 in the 64-bit PEB");
static_assert(sizeof(PebLayout) == 0x7A0, "128 pointer-sized buckets");

PebLayout* CurrentPeb() {
  return reinterpret_cast<PebLayout*>(NtCurrentTeb()->ProcessEnvironmentBlock);
}

// A bucket that holds nothing, or a wait block where one can be.
bool PlausibleBucket(void* entry) {
  const uintptr_t block = reinterpret_cast<uintptr_t>(entry) & ~uintptr_t{3};  // the flag bits
  if (block == 0) return true;
  if ((block & 7) != 0) return false;
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(reinterpret_cast<void*>(block), &info, sizeof(info)) != sizeof(info)) return false;
  return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE;
}
#endif

// In the parent, with the fork set held: whether the child's table can be
// emptied (above).
bool WaitTableUsable() {
#if defined(_WIN64)
  using VersionFn = void(NTAPI*)(ULONG*, ULONG*, ULONG*);
  static auto version = reinterpret_cast<VersionFn>(Ntdll("RtlGetNtVersionNumbers"));
  if (version == nullptr) return false;
  ULONG major = 0, minor = 0, build = 0;
  version(&major, &minor, &build);
  if (major < 6 || (major == 6 && minor < 2)) return false;
  for (void* entry : CurrentPeb()->wait_on_address_hash_table) {
    if (!PlausibleBucket(entry)) return false;
  }
  return true;
#else
  return false;
#endif
}

// In the child, before anything else runs in it.
void EmptyWaitTable() {
#if defined(_WIN64)
  for (void* volatile& entry : CurrentPeb()->wait_on_address_hash_table) {
    void* value = entry;
    if (value != nullptr && PlausibleBucket(value)) entry = nullptr;
  }
#endif
}

// Object types a clone must not share with this process: the thread pool's
// worker factory and its completion ports and wait packets (which would run
// the child's work on this process's threads), ALPC ports (this process's
// connections to system services) and the graphics kernel's objects.
bool Excluded(const std::wstring& type) {
  static const wchar_t* const kTypes[] = {
      L"TpWorkerFactory", L"IoCompletion", L"IoCompletionReserve", L"WaitCompletionPacket",
      L"ALPC Port", L"EtwRegistration", L"EtwConsumer"};
  for (const wchar_t* t : kTypes) {
    if (type == t) return true;
  }
  return type.rfind(L"Dxgk", 0) == 0 || type.rfind(L"Composition", 0) == 0;
}

std::wstring TypeName(HANDLE handle) {
  static auto query = reinterpret_cast<QueryObjectFn>(Ntdll("NtQueryObject"));
  alignas(8) BYTE buffer[1024];
  ULONG size = 0;
  // ObjectTypeInformation (2): PUBLIC_OBJECT_TYPE_INFORMATION, a UNICODE_STRING first.
  if (query == nullptr || query(handle, 2, buffer, sizeof(buffer), &size) < 0) return L"";
  const auto* name = reinterpret_cast<const UNICODE_STRING*>(buffer);
  return std::wstring(name->Buffer, name->Length / sizeof(wchar_t));
}

// A handle made inheritable for a clone, with what identifies its object
// while it stays open: its type and granted access.
struct Changed {
  HANDLE value;
  ULONG type_index;
  ACCESS_MASK access;
};

bool Snapshot(std::vector<BYTE>* buffer) {
  static auto query = reinterpret_cast<QueryProcessFn>(Ntdll("NtQueryInformationProcess"));
  if (query == nullptr) return errno = ENOSYS, false;
  buffer->resize(64 * 1024);
  for (;;) {
    ULONG size = 0;
    LONG status = query(GetCurrentProcess(), 51, buffer->data(), static_cast<ULONG>(buffer->size()),
                        &size);
    if (status >= 0) return true;
    if (status != static_cast<LONG>(0xC0000004) /* STATUS_INFO_LENGTH_MISMATCH */ ||
        buffer->size() > (64u << 20)) {
      return errno = ENOMEM, false;
    }
    buffer->resize(std::max<size_t>(buffer->size() * 2, size + 4096));
  }
}

// The handles made inheritable for a clone, to be given back their flag.
bool InheritAllHandles(std::vector<Changed>* changed) {
  std::vector<BYTE> buffer;
  if (!Snapshot(&buffer)) return false;
  const auto* snapshot = reinterpret_cast<const HandleSnapshot*>(buffer.data());
  std::vector<std::pair<ULONG, bool>> types;  // type index -> excluded
  for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
    const HandleEntry& entry = snapshot->handles[i];
    if ((entry.attributes & kObjInherit) != 0) continue;
    // The spawn broker's pipe instances stay the root's: a clone holding one
    // would keep it open after the root ends, and a request would wait there.
    if (gesher::IsBrokerHandle(entry.value)) continue;
    bool excluded = false, known = false;
    for (const auto& t : types) {
      if (t.first == entry.type_index) excluded = t.second, known = true;
    }
    if (!known) {
      excluded = Excluded(TypeName(entry.value));
      types.emplace_back(entry.type_index, excluded);
    }
    if (excluded) continue;
    if (SetHandleInformation(entry.value, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
      changed->push_back({entry.value, entry.type_index, entry.granted_access});
    }
  }
  return true;
}

// Gives each handle back its flag if the value still names an object of the
// same type and access: another thread may have closed one meanwhile and
// opened another, inheritable on purpose, at the same value. (Comparing the
// objects themselves would cost a duplicate of every handle.)
void RestoreHandles(const std::vector<Changed>& changed) {
  if (changed.empty()) return;
  std::vector<BYTE> buffer;
  if (!Snapshot(&buffer)) {
    for (const Changed& c : changed) SetHandleInformation(c.value, HANDLE_FLAG_INHERIT, 0);
    return;
  }
  const auto* snapshot = reinterpret_cast<const HandleSnapshot*>(buffer.data());
  std::unordered_map<HANDLE, const HandleEntry*> now;
  now.reserve(static_cast<size_t>(snapshot->count));
  for (ULONG_PTR i = 0; i < snapshot->count; ++i) now[snapshot->handles[i].value] = &snapshot->handles[i];
  for (const Changed& c : changed) {
    auto it = now.find(c.value);
    if (it == now.end() || it->second->type_index != c.type_index ||
        it->second->granted_access != c.access) {
      continue;
    }
    SetHandleInformation(c.value, HANDLE_FLAG_INHERIT, 0);
  }
}

// Gesher's own spawns hold this shared around CreateProcess (they pass
// explicit handle lists anyway); a fork holds it exclusively while every
// handle is inheritable.
SRWLOCK g_spawn_lock = SRWLOCK_INIT;

// The parent's references to its forked children's mailboxes: the child
// opens the same named objects, and a signal sent before it does waits there.
struct ChildMailbox {
  DWORD pid;
  HANDLE section;
  HANDLE event;
};
std::mutex g_child_mailboxes_mutex;
std::vector<ChildMailbox> g_child_mailboxes;

void PrepareChildMailbox(DWORD pid, HANDLE process) {
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Mailbox), MailboxName(pid).c_str());
  if (section == nullptr) return;
  if (auto* mailbox = static_cast<Mailbox*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Mailbox)))) {
    ClaimMailbox(mailbox, CreationTime(process), true);
    UnmapViewOfFile(mailbox);
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, EventName(pid).c_str());
  // g_child_mailboxes_mutex is in the fork set, which the caller holds.
  g_child_mailboxes.push_back({pid, section, event});
}

}  // namespace

GESHER_FORK_LOCK(gesher::kForkOrderProcess, std::mutex, g_child_mailboxes_mutex);
GESHER_FORK_LOCK(gesher::kForkOrderLeaf, std::mutex, g_limits_mutex);

namespace gesher {

void AcquireSpawnLock() { AcquireSRWLockShared(&g_spawn_lock); }
void ReleaseSpawnLock() { ReleaseSRWLockShared(&g_spawn_lock); }

bool InForkedChild() { return g_forked_child.load(std::memory_order_acquire); }

bool UsesForkExitCodes() { return InForkedChild() || g_fork_exit_codes; }

DWORD SignalExitCode(int sig) {
  return UsesForkExitCodes() ? ForkSignalExitCode(sig) : 128 + static_cast<DWORD>(sig);
}

bool IsForkedChild(DWORD pid) {
  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, MailboxName(pid).c_str());
  if (section == nullptr) return false;
  bool forked = false;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (auto* mailbox = static_cast<Mailbox*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(Mailbox)))) {
    forked = mailbox->forked != 0 && process != nullptr && mailbox->owner_created == CreationTime(process);
    UnmapViewOfFile(mailbox);
  }
  if (process != nullptr) CloseHandle(process);
  CloseHandle(section);
  return forked;
}

void SetExecForward(DWORD pid) { g_exec_forward.store(pid, std::memory_order_release); }

void ForgetChildMailbox(DWORD pid) {
  ForgetForkedChild(pid);
  std::lock_guard<std::mutex> lock(g_child_mailboxes_mutex);
  for (auto it = g_child_mailboxes.begin(); it != g_child_mailboxes.end(); ++it) {
    if (it->pid != pid) continue;
    if (it->section != nullptr) CloseHandle(it->section);
    if (it->event != nullptr) CloseHandle(it->event);
    g_child_mailboxes.erase(it);
    return;
  }
}

int CloneProcess(HANDLE* process, DWORD* pid) {
  static auto clone = reinterpret_cast<CloneFn>(Ntdll("RtlCloneUserProcess"));
  if (clone == nullptr) return errno = ENOSYS, -1;
  // The child's address-wait table must be emptied, and can be only when its
  // layout checks out (above).
  if (!WaitTableUsable()) return errno = EAGAIN, -1;
  std::vector<Changed> changed;
  changed.reserve(1024);
  AcquireSRWLockExclusive(&g_spawn_lock);
  if (!InheritAllHandles(&changed)) {
    int error = errno;
    RestoreHandles(changed);
    ReleaseSRWLockExclusive(&g_spawn_lock);
    return errno = error, -1;
  }
  RtlProcessInformation info{};
  info.length = sizeof(info);
  LONG status = clone(kCloneInheritHandles, nullptr, nullptr, nullptr, &info);
  if (status == kStatusProcessCloned) {
    // First: no wake in the child may go to a parent's waiter (above).
    EmptyWaitTable();
    g_forked_child.store(true, std::memory_order_release);
    // The child's copies: their own flags again, at the same values.
    RestoreHandles(changed);
    InitializeSRWLock(&g_spawn_lock);
    // The parent's references to its own children's mailboxes are not ours.
    for (ChildMailbox& m : g_child_mailboxes) {
      if (m.section != nullptr) CloseHandle(m.section);
      if (m.event != nullptr) CloseHandle(m.event);
    }
    g_child_mailboxes.clear();
    return 0;
  }
  RestoreHandles(changed);
  ReleaseSRWLockExclusive(&g_spawn_lock);
  if (status < 0) {
    // STATUS_NO_MEMORY, STATUS_COMMITMENT_LIMIT, quota: EAGAIN/ENOMEM as fork(2).
    return errno = (status == static_cast<LONG>(0xC0000017) || status == static_cast<LONG>(0xC000012D))
                       ? ENOMEM
                       : EAGAIN,
           -1;
  }
  CloseHandle(info.thread);
  *process = info.process;
  *pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(info.client_id.UniqueProcess));
  // Before fork returns: a signal the parent sends the new pid at once waits
  // in this mailbox until the child watches it.
  PrepareChildMailbox(*pid, *process);
  // A forked descendant of the broker's root (spawn_broker.cc).
  RegisterForkedChild(*pid, *process);
  return 1;
}

void ReinitializeAfterFork() {
  // IMM32's thread-attach code reads the session's win32k shared memory,
  // which a clone does not map: every thread the child starts would fault
  // there. The child has no windows; IMM32 needs no per-thread state in it.
  if (HMODULE imm = GetModuleHandleW(L"imm32.dll")) DisableThreadLibraryCalls(imm);
  // The parent's mailbox view is shared memory: the child must not take the
  // parent's signals. Its own mailbox is named by its own pid.
  g_mailbox = nullptr;
  OpenMailbox(false);
  // The child's own ptrace state (ADR 0093): its section, named by its pid,
  // with the dumpable flag the parent had at the clone and no tracer or
  // ptracer exception, and its own guard, so that a tracer can stop it from
  // the moment fork returns in it. This is the executable's copy of
  // gesher-base, the one that owns the guard.
  gesher::ptrace::Self();
}

}  // namespace gesher
