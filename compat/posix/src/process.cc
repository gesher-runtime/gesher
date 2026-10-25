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

#include <mutex>
#include <string>

#include "internal.h"

namespace {

struct Mailbox {
  volatile LONG64 pending;  // bit (signal - 1)
  volatile LONG senders[NSIG];
};

std::wstring MailboxName(DWORD pid) { return L"Local\\gesher-signals-" + std::to_wstring(pid); }
std::wstring EventName(DWORD pid) { return L"Local\\gesher-signal-" + std::to_wstring(pid); }

Mailbox* g_mailbox = nullptr;

bool DefaultIgnores(int sig) {
  return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

void Deliver(int sig) {
  struct sigaction sa = {};
  gesher_sigaction_raw(sig, nullptr, &sa);
  if (sa.sa_handler == SIG_IGN) return;
  if (sa.sa_handler == SIG_DFL) {
    if (DefaultIgnores(sig)) return;
    if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP) {
      TerminateProcess(GetCurrentProcess(), 128 + sig);
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

// The mailbox of this process, made at startup (posix.cmake keeps this file
// linked into every executable).
const bool g_mailbox_ready = [] {
  DWORD pid = GetCurrentProcessId();
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Mailbox), MailboxName(pid).c_str());
  if (section == nullptr) return false;
  g_mailbox = static_cast<Mailbox*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Mailbox)));
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, EventName(pid).c_str());
  HANDLE wait = nullptr;
  if (g_mailbox == nullptr || event == nullptr ||
      !RegisterWaitForSingleObject(&wait, event, OnSignal, nullptr, INFINITE, WT_EXECUTEDEFAULT)) {
    return false;
  }
  return true;  // the section and event stay open for the life of the process
}();

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
    if (!TerminateProcess(process, 128 + SIGKILL)) result = gesher::FailWithLastError();
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

}  // namespace

int getrlimit(int resource, struct rlimit* rlim) {
  if (resource < 0 || resource >= kLimits) return errno = EINVAL, -1;
  if (rlim == nullptr) return errno = EFAULT, -1;
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
