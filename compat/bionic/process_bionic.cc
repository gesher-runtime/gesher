// gesher bionic shim: creating processes from native code (docs/exec.md,
// docs/decisions/0055-exec2-native-processes.md).
//
// Native programs are launched with CreateProcessW. system(), popen() and
// posix_spawn() do not require an address-space clone. exec*() retains the
// launcher's documented process-id limitation, while preserving inherited
// descriptors and their shared open file descriptions.
//
// Windows cannot provide fork() or vfork() through an inline callback: that
// would expose the parent's memory, thread state and descriptor table to a
// purported child. Both calls fail with ENOSYS until a real implementation is
// available. Reads and writes always transfer the actual bytes supplied by
// their peers; there are no application-specific process or pipe exceptions.
//
// wait()/waitpid()/wait4() collect actual spawned children and report Linux
// status words. A child's exit raises SIGCHLD.
//
// exec_spawn.h finds the program and decides how it runs (an ELF executable in
// gesher-exec, a PE as itself, sh/scripts/tools in the launcher).

#include "shim_internal.h"
#include "program_break.h"

#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>
#include <gesher/environment.h>
#include <gesher/fork.h>
#include <gesher/environment_utf16.h>
#include <gesher/logging.h>

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <psapi.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "exec_spawn.h"
#include "spawn_actions.h"
#include "system_cacerts.h"

// gesher-base: kill() of a process other than this one's children (signal
// mailbox, compat/posix/src/process.cc).
extern "C" int gesher_kill_other(int pid, int sig);

namespace ex = gesher::exec;

namespace gesher::bionic {

namespace {

constexpr int kSigchld = 17;
constexpr int kCldExited = 1;  // siginfo si_code for a normal child exit
constexpr int kCldKilled = 2;  // ... for a child a signal ended

// --- the shim's children -------------------------------------------------------

struct Child {
  HANDLE process = nullptr;
  bool exited = false;
  bool notified = false;  // its exit signal was sent (or it has none)
  int status = 0;  // Linux waitpid status word, once it exited
  HANDLE wait = nullptr;
  bool watched = false;
  // The signal its exit sends the parent: SIGCHLD for spawned and forked
  // children; clone(2)'s CSIGNAL for a raw clone (0: none). A child whose
  // exit signal is not SIGCHLD is a "clone" child for wait's __WCLONE/__WALL.
  int exit_signal = kSigchld;
  bool forked = false;  // a fork's child: its exit code encodes a signal death distinctly
};

std::mutex g_children_mutex;
std::map<int, Child> g_children;  // pid -> child
std::atomic<uint64_t> g_child_wait_generation{0};

// Under g_children_mutex: records the exit of a child whose process ended.
// Returns the signal to send for it (0 if none or already sent).
// A forked child's exit code: gesher::ForkSignalExitCode for a death by a
// signal, else its exit status (0..255), or a Windows exception code.
int ForkedWaitStatus(DWORD code) { return gesher::ForkedWaitStatusOf(code, &ex::WaitStatus); }

int RecordExitLocked(Child& child) {
  if (!child.exited) {
    DWORD code = 0;
    GetExitCodeProcess(child.process, &code);
    // A process that uses the forked child's exit codes decodes every child's
    // status with them: its children use them too (gesher/fork.h).
    child.status = child.forked || gesher::UsesForkExitCodes() ? ForkedWaitStatus(code) : ex::WaitStatus(code);
    child.exited = true;
  }
  if (child.notified) return 0;
  child.notified = true;
  return child.exit_signal;
}

void SendExitSignal(int pid, int sig, int status) {
  if (sig == 0) return;
  const bool killed = (status & 0x7f) != 0;
  DeliverExternalSignal(sig, pid, killed ? kCldKilled : kCldExited,
                        killed ? (status & 0x7f) : ((status >> 8) & 0xff), false);
}

// Under g_children_mutex: forgets a child that has been reaped.
void ReleaseChildLocked(int pid, Child& child) {
  if (child.wait != nullptr) UnregisterWaitEx(child.wait, nullptr);
  if (child.process != nullptr) CloseHandle(child.process);
  if (child.forked) gesher::ForgetChildMailbox(static_cast<DWORD>(pid));
}

void ChildExited(int pid) {
  int sig = 0, status = 0;
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(pid);
    if (it == g_children.end() || it->second.exited ||
        WaitForSingleObject(it->second.process, 0) != WAIT_OBJECT_0) return;
    sig = RecordExitLocked(it->second);
    status = it->second.status;
    // SIGCHLD ignored or SA_NOCLDWAIT: Linux reaps the child at once (no
    // zombie for wait to report).
    if (ChildrenAutoReaped()) {
      ReleaseChildLocked(it->first, it->second);
      g_children.erase(it);
    }
  }
  SendExitSignal(pid, sig, status);
  ChildWaitStateChanged();
}

void CALLBACK OnChildExit(void* param, BOOLEAN) {
  ChildExited(static_cast<int>(reinterpret_cast<intptr_t>(param)));
}

// A forked child has no thread pool (gesher/fork.h): a thread of its own
// waits for each of its children, on a handle of its own.
struct Watch {
  int pid;
  HANDLE process;
};
DWORD WINAPI WatchChild(void* raw) {
  Watch watch = *static_cast<Watch*>(raw);
  delete static_cast<Watch*>(raw);
  WaitForSingleObject(watch.process, INFINITE);
  std::lock_guard<std::mutex> helper(gesher::ForkHelperLock());  // not while a fork clones
  CloseHandle(watch.process);
  ChildExited(watch.pid);
  return 0;
}

bool StartChildWatcher(int pid, Child& child) {
  if (!gesher::InForkedChild()) {
    return RegisterWaitForSingleObject(&child.wait, child.process, OnChildExit,
                                       reinterpret_cast<void*>(static_cast<intptr_t>(pid)), INFINITE,
                                       WT_EXECUTEONLYONCE) != FALSE;
  }
  HANDLE own = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), child.process, GetCurrentProcess(), &own, SYNCHRONIZE, FALSE, 0)) {
    return false;
  }
  auto* watch = new (std::nothrow) Watch{pid, own};
  if (watch != nullptr && gesher::StartHelperThread(WatchChild, watch, 64 * 1024)) return true;
  delete watch;
  CloseHandle(own);
  return false;
}

void Track(int pid, HANDLE process, int exit_signal = kSigchld, bool forked = false) {
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    Child& c = g_children[pid];
    c = Child{};
    c.process = process;
    c.exit_signal = exit_signal;
    c.forked = forked;
    c.watched = StartChildWatcher(pid, c);
  }
  ChildWaitStateChanged();
}

// --- the environment and working directory as UTF-8 ----------------------------

bool CurrentEnv(std::vector<std::string>* out) {
  return gesher::VisitEnvironment([](const char* entry, void* context) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(entry);
  }, out);
}

std::string CurrentDir() {
  DWORD n = GetCurrentDirectoryW(0, nullptr);
  std::wstring d(n, L'\0');
  n = GetCurrentDirectoryW(n, d.data());
  d.resize(n);
  return ex::Narrow(d);
}

// The null-terminated argv/envp a .so passes, as vectors.
std::vector<std::string> FromList(char* const list[]) {
  std::vector<std::string> out;
  if (list != nullptr) {
    for (int i = 0; list[i] != nullptr; ++i) out.push_back(list[i]);
  }
  return out;
}

// The existing descriptor entries remain authoritative. close_range uses
// their guest ABI adapters without replacing ordinary fd or I/O operations.
using CloseFn = int (BIONIC_SYSV*)(int);
using FcntlFn = int (BIONIC_SYSV*)(int, int, uint64_t);
CloseFn g_close = nullptr;
FcntlFn g_fcntl = nullptr;

// --- spawning ------------------------------------------------------------------

// Builds the child fd set, environment and cwd, spawns `argv[0]`, tracks the
// pid. `search`: look argv[0] up in PATH (execvp/posix_spawnp). Returns the
// pid, or -1 with errno set.
int SpawnPrepared(const ex::Launch& launch, const std::vector<std::string>& env,
                   const std::string& dir, const std::vector<ex::ChildFd>& fds, HANDLE job = nullptr) {
  std::wstring environment = ex::EnvironmentBlock(env);
  if (environment.empty()) return -1;
  std::wstring cwd = dir.empty() ? L"" : ex::WindowsPath(dir);
  PROCESS_INFORMATION pi = {};
  DWORD werr = ex::SpawnWithFds(launch, environment.c_str(), cwd, fds, &pi, job != nullptr ? CREATE_SUSPENDED : 0);
  if (werr != 0) return errno = ex::ErrnoFromWin32(werr), -1;
  if (job != nullptr) {
    // In `job` before it runs: anything it starts is in the job too.
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
      const DWORD error = GetLastError();
      TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return errno = ex::ErrnoFromWin32(error), -1;
    }
    ResumeThread(pi.hThread);
  }
  CloseHandle(pi.hThread);
  int pid = static_cast<int>(pi.dwProcessId);
  Track(pid, pi.hProcess);
  return pid;
}

int SpawnProgram(const std::string& program, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, const std::string& dir, bool search,
                 const std::vector<ex::ChildFd>& fds, HANDLE job = nullptr) {
  ex::Launch launch;
  int e = ex::PrepareLaunchProgram(program, argv, dir, search, &launch);
  if (e != 0) return errno = e, -1;
  return SpawnPrepared(launch, env, dir, fds, job);
}

// Pin descriptors and their shared status before releasing the descriptor
// transaction. A concurrent close/reuse cannot change what the child inherits.
// posix_spawn includes CLOEXEC entries while applying file actions; the child
// closes the remaining CLOEXEC entries immediately before executing its program.
bool InheritedFds(std::vector<ex::ChildFd>* out, bool include_close_on_exec = false) {
  std::vector<std::pair<int, DescriptorSnapshotPtr>> captured;
  if (CaptureDescriptors(&captured, include_close_on_exec) != 0) return false;
  out->reserve(captured.size());
  for (auto& [fd, snapshot] : captured) {
    out->push_back({fd, snapshot->handle, std::move(snapshot)});
  }
  return true;
}

int BIONIC_SYSV ForkUnsupported() { return errno = ENOSYS, -1; }

int BIONIC_SYSV DaemonUnsupported(int, int) { return errno = ENOSYS, -1; }

ProgramBreak& ProcessBreak() { static ProgramBreak value; return value; }
void* BIONIC_SYSV Sbrk(intptr_t increment) { return ProcessBreak().Increment(increment); }
int BIONIC_SYSV Brk(void* address) { return ProcessBreak().Set(address); }

// --- execve / execvp and friends ----------------------------------------------

// Returns only on failure (errno set); on success it exits the process.
void ExecReplace(const std::string& program, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, bool search) {
  std::vector<ex::ChildFd> fds;
  if (!InheritedFds(&fds)) return;
  // A forked child's program replaces it (its signal deaths use the forked
  // child's exit codes: it learns that from this process's mailbox). It runs
  // in a job that ends with this process, so a SIGKILL of this pid ends the
  // program too; other signals sent here are forwarded to it.
  HANDLE job = nullptr;
  if (gesher::UsesForkExitCodes()) {
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // Only the program itself is in the job: what it starts breaks away
    // silently, so it outlives a SIGKILL of this pid, as on Linux.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (job == nullptr ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
      if (job != nullptr) CloseHandle(job);
      fds.clear();
      errno = EAGAIN;
      return;
    }
  }
  int pid = SpawnProgram(program, argv, env, CurrentDir(), search, fds, job);
  fds.clear();  // snapshots must not keep child pipe endpoints open while waiting
  if (pid < 0) {
    if (job != nullptr) {
      const int error = errno;
      CloseHandle(job);
      errno = error;
    }
    return;
  }
  if (job != nullptr) gesher::SetExecForward(static_cast<DWORD>(pid));
  HANDLE h = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(pid);
    if (it != g_children.end()) h = it->second.process;
  }
  if (h != nullptr) {
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    if (gesher::UsesForkExitCodes()) {
      // The forked child "became" the program: its parent sees the program's
      // status, a death by a signal included (gesher/fork.h's exit codes).
      const int status = ForkedWaitStatus(code);
      const UINT exit_code = (status & 0x7f) != 0 ? gesher::ForkSignalExitCode(status & 0x7f)
                                                  : static_cast<UINT>((status >> 8) & 0xff);
      if (gesher::InForkedChild()) TerminateProcess(GetCurrentProcess(), exit_code);
      std::fflush(nullptr);
      ExitProcess(exit_code);
    }
    std::fflush(nullptr);
    ExitProcess(static_cast<UINT>(ex::ExitStatus(code)) & 0xff);
  }
  if (gesher::InForkedChild()) TerminateProcess(GetCurrentProcess(), 1);
  ExitProcess(1);
}

int DoExec(const char* path, char* const argv[], char* const envp[], bool search) {
  if (path == nullptr || argv == nullptr) return errno = EFAULT, -1;
  std::vector<std::string> args = FromList(argv);
  if (args.empty()) args.push_back(path);
  std::vector<std::string> env;
  if (envp) env = FromList(envp);
  else if (!CurrentEnv(&env)) return -1;
  ExecReplace(path, args, env, search);  // returns only on failure
  return -1;
}

int BIONIC_SYSV Execve(const char* path, char* const argv[], char* const envp[]) {
  return DoExec(path, argv, envp, false);
}
int BIONIC_SYSV Execv(const char* path, char* const argv[]) {
  return DoExec(path, argv, nullptr, false);
}
int BIONIC_SYSV Execvp(const char* file, char* const argv[]) {
  return DoExec(file, argv, nullptr, true);
}
int BIONIC_SYSV Execvpe(const char* file, char* const argv[], char* const envp[]) {
  return DoExec(file, argv, envp, true);
}

// The list forms take argv as variadic pointers up to a null one; execle's
// envp follows that null (bionic's execl.cpp). The System V trampoline and the
// ARM64 bridge both supply the argument list after path and argv[0] (NextGp
// reads either va_list); the vector forms share path resolution, environment
// and errors.
std::vector<char*> ListArgv(const char* first, SysvVaList* args) {
  std::vector<char*> argv{const_cast<char*>(first)};
  if (first != nullptr) {
    while (auto* argument = reinterpret_cast<char*>(NextGp(args))) {
      argv.push_back(argument);
    }
    argv.push_back(nullptr);
  }
  return argv;
}

extern "C" int BIONIC_SYSV gesher_bionic_vexecl(const char* path, const char* first,
                                               SysvVaList* args) {
  std::vector<char*> argv = ListArgv(first, args);
  return Execv(path, argv.data());
}
extern "C" int BIONIC_SYSV gesher_bionic_vexeclp(const char* file, const char* first,
                                                SysvVaList* args) {
  std::vector<char*> argv = ListArgv(first, args);
  return Execvp(file, argv.data());
}
extern "C" int BIONIC_SYSV gesher_bionic_vexecle(const char* path, const char* first,
                                                SysvVaList* args) {
  std::vector<char*> argv = ListArgv(first, args);
  auto envp = reinterpret_cast<char* const*>(NextGp(args));
  // A null envp is an empty environment for execve, not the caller's.
  static char* const kEmpty[] = {nullptr};
  return Execve(path, argv.data(), envp != nullptr ? envp : kEmpty);
}

// Two fixed arguments (path or file, argv[0]), the va_list in %rdx.
extern "C" void gesher_bionic_execl();
extern "C" void gesher_bionic_execlp();
extern "C" void gesher_bionic_execle();
asm(GESHER_VA_TRAMPOLINE("gesher_bionic_execl", "gesher_bionic_vexecl", "16", "%rdx"));
asm(GESHER_VA_TRAMPOLINE("gesher_bionic_execlp", "gesher_bionic_vexeclp", "16", "%rdx"));
asm(GESHER_VA_TRAMPOLINE("gesher_bionic_execle", "gesher_bionic_vexecle", "16", "%rdx"));

// --- waitpid / wait / wait4 ----------------------------------------------------

constexpr int kWNOHANG = 1;
constexpr int kWUNTRACED = 2;
constexpr int kWCONTINUED = 8;
constexpr int kWNOTHREAD = 0x20000000;
constexpr int kWALL = 0x40000000;
constexpr int kWCLONE = static_cast<int>(0x80000000u);
constexpr int kWaitOptions = kWNOHANG | kWUNTRACED | kWCONTINUED | kWNOTHREAD | kWALL | kWCLONE;

// bionic's LP64 struct rusage (the same on x86-64 and arm64).
struct BionicRusage {
  int64_t utime_sec, utime_usec, stime_sec, stime_usec;
  int64_t maxrss, ixrss, idrss, isrss, minflt, majflt, nswap, inblock, oublock, msgsnd, msgrcv,
      nsignals, nvcsw, nivcsw;
};
static_assert(sizeof(BionicRusage) == 144);

void FillRusage(HANDLE process, BionicRusage* usage) {
  std::memset(usage, 0, sizeof(*usage));
  FILETIME created, exited, kernel, user;
  if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    auto split = [](FILETIME ft, int64_t* sec, int64_t* usec) {
      const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
      *sec = static_cast<int64_t>(t / 10'000'000);
      *usec = static_cast<int64_t>((t % 10'000'000) / 10);
    };
    split(user, &usage->utime_sec, &usage->utime_usec);
    split(kernel, &usage->stime_sec, &usage->stime_usec);
  }
  PROCESS_MEMORY_COUNTERS counters = {};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
    usage->maxrss = static_cast<int64_t>(counters.PeakWorkingSetSize / 1024);
    usage->minflt = counters.PageFaultCount;
  }
}

// Whether wait with `options` takes this child (Linux's eligible_child): a
// child whose exit signal is not SIGCHLD only with __WCLONE, the others only
// without it, and every child with __WALL.
bool Eligible(const Child& child, int options) {
  if ((options & kWALL) != 0) return true;
  return (child.exit_signal != kSigchld) == ((options & kWCLONE) != 0);
}

bool IsChild(int pid) {
  std::lock_guard<std::mutex> lock(g_children_mutex);
  return g_children.count(pid) != 0;
}

struct ChildExitSignal { int pid, signal, status; };

// The children of this process are its spawned and forked ones; process
// groups are not modelled (0 and < -1 take any child, as -1).
int CollectOne(int pid, int* status, int options, BionicRusage* usage = nullptr) {
  if ((options & ~kWaitOptions) != 0) return errno = EINVAL, -1;
  unsigned waited_slices = 0;
  bool interrupted = false;  // a handler without SA_RESTART ran during the wait
  for (;;) {
    // Capture before scanning either registry: a publisher can change state
    // after its scan and before this thread arms its wait.
    const uint64_t observed = g_child_wait_generation.load(std::memory_order_acquire);
    // A tracer's tracees report their stops and exits too (ADR 0093); a
    // tracee that is also a child reports its exit as a child, below.
    bool traced = false;
    if (int got = PtraceWaitPoll(pid, options, status, &traced, &IsChild); got != 0) return got;
    int sig = 0, got = 0, word = 0;
    bool found = false;
    bool needs_poll = traced;
    bool removed = false;
    std::vector<ChildExitSignal> auto_reaped_signals;
    {
      std::lock_guard<std::mutex> lock(g_children_mutex);
      for (auto it = g_children.begin(); it != g_children.end();) {
        if ((pid > 0 && it->first != pid) || !Eligible(it->second, options)) {
          ++it;
          continue;
        }
        Child& child = it->second;
        if (!child.exited && WaitForSingleObject(child.process, 0) != WAIT_OBJECT_0) {
          found = true;
          needs_poll |= !child.watched;
          ++it;
          continue;
        }
        // Exited: reported once its process ended, whether or not its
        // watcher has run yet; the watcher's exit signal is sent here then.
        sig = RecordExitLocked(child);
        if (ChildrenAutoReaped()) {
          // Reaped at its exit (SIGCHLD ignored, SA_NOCLDWAIT): never reported.
          if (sig != 0) auto_reaped_signals.push_back({it->first, sig, child.status});
          sig = 0;
          ReleaseChildLocked(it->first, child);
          it = g_children.erase(it);
          removed = true;
          continue;
        }
        found = true;
        got = it->first;
        word = child.status;
        if (usage != nullptr) FillRusage(child.process, usage);
        ReleaseChildLocked(it->first, child);
        g_children.erase(it);
        removed = true;
        break;
      }
    }
    for (const auto& exited : auto_reaped_signals) SendExitSignal(exited.pid, exited.signal, exited.status);
    if (removed) ChildWaitStateChanged();
    if (!found && !traced) return errno = 10 /*ECHILD*/, -1;
    if (got != 0) {
      if (status != nullptr) *status = word;
      SendExitSignal(got, sig, word);
      // A child's exit signal its watcher had not sent yet is handled here,
      // as Linux runs it on the wait's return: left pending, it would
      // interrupt this thread's next wait (for another child) with EINTR.
      DeliverPendingSignals();
      return got;
    }
    if ((options & kWNOHANG) != 0) return 0;
    if (interrupted) {
      // A handler interrupted the wait, and the rescan found no child that
      // ended (Linux's do_wait finds an exited child before it returns EINTR).
      return errno = kEINTR, -1;
    }
    // Remote ptrace stops and children without a watcher have no local exit
    // callback to wake us. Retain the current polling cadence for them only.
    const uint32_t timeout = needs_poll ? (waited_slices++ < 50 ? 2 : 20) : 0;
    const int error = InterruptibleWaitPoint(timeout, g_child_wait_generation, observed);
    if (error == kEINTR) {
      interrupted = true;  // look at the children once more first
    } else if (error != 0) {
      return errno = error, -1;
    }
  }
}

int BIONIC_SYSV Waitpid(int pid, int* status, int options) { return CollectOne(pid, status, options); }
int BIONIC_SYSV Wait(int* status) { return CollectOne(-1, status, 0); }
int BIONIC_SYSV Wait4(int pid, int* status, int options, void* rusage) {
  return CollectOne(pid, status, options, static_cast<BionicRusage*>(rusage));
}

// --- system / popen ------------------------------------------------------------

int BIONIC_SYSV System(const char* command) {
  if (command == nullptr) return 1;  // a shell is available
  std::vector<std::string> argv{"/system/bin/sh", "-c", command};
  std::vector<ex::ChildFd> fds;
  if (!InheritedFds(&fds)) return -1;
  std::vector<std::string> env;
  if (!CurrentEnv(&env)) return -1;
  int pid = SpawnProgram("/system/bin/sh", argv, env, CurrentDir(), false, fds);
  fds.clear();
  if (pid < 0) return -1;
  int status = 0;
  int collected;
  do collected = CollectOne(pid, &status, 0); while (collected < 0 && errno == EINTR);
  if (collected < 0) return -1;
  return status;
}

std::mutex g_popen_mutex;
std::map<void*, int> g_popen;  // FILE* -> child pid

void* BIONIC_SYSV Popen(const char* command, const char* mode) {
  if (command == nullptr || mode == nullptr || (*mode != 'r' && *mode != 'w')) {
    return errno = EINVAL, nullptr;
  }
  bool read = *mode == 'r';
  HANDLE child_end = nullptr, parent_end = nullptr;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  // read: child writes to its stdout, parent reads; write: the other way.
  if (!CreatePipe(read ? &parent_end : &child_end, read ? &child_end : &parent_end, &sa, 256 * 1024)) {
    return errno = EMFILE, nullptr;
  }
  if (!SetHandleInformation(parent_end, HANDLE_FLAG_INHERIT, 0)) {
    int error = ex::ErrnoFromWin32(GetLastError());
    CloseHandle(child_end);
    CloseHandle(parent_end);
    return errno = error, nullptr;
  }
  auto child_description = gesher::CaptureFileHandle(child_end);
  auto parent_description = gesher::CaptureFileHandle(parent_end);
  int fd = -1;
  if (!child_description || !parent_description ||
      gesher::InitializePipeDescription(child_description, read ? O_WRONLY : O_RDONLY) < 0 ||
      gesher::InitializePipeDescription(parent_description, read ? O_RDONLY : O_WRONLY) < 0 ||
      (fd = gesher::AttachFileDescription(parent_description, true)) < 0) {
    int error = errno;
    CloseHandle(child_end);
    CloseHandle(parent_end);
    return errno = error, nullptr;
  }
  CloseHandle(parent_end);
  parent_description.reset();
  std::vector<ex::ChildFd> fds;
  if (!InheritedFds(&fds)) {
    int error = errno;
    CloseHandle(child_end);
    ::close(fd);
    return errno = error, nullptr;
  }
  // Replace the child's stdout (read) or stdin (write) with the pipe.
  int target = read ? 1 : 0;
  for (auto& f : fds) {
    if (f.fd == target) f = {target, child_description->handle, child_description};
  }
  bool found = false;
  for (auto& f : fds) found = found || f.fd == target;
  if (!found) fds.push_back({target, child_description->handle, child_description});
  std::vector<std::string> argv{"/system/bin/sh", "-c", command};
  std::vector<std::string> env;
  int pid = CurrentEnv(&env) ? SpawnProgram("/system/bin/sh", argv, env, CurrentDir(), false, fds) : -1;
  fds.clear();
  CloseHandle(child_end);
  child_description.reset();
  if (pid < 0) {
    int error = errno;
    ::close(fd);
    errno = error;
    return nullptr;
  }
  FILE* f = _fdopen(fd, read ? "rb" : "wb");
  if (f == nullptr) {
    int error = errno;
    ::close(fd);
    errno = error;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_popen_mutex);
  g_popen[f] = pid;
  return f;
}

int BIONIC_SYSV Pclose(void* stream) {
  if (stream == nullptr) return errno = EINVAL, -1;
  int pid = 0;
  {
    std::lock_guard<std::mutex> lock(g_popen_mutex);
    auto it = g_popen.find(stream);
    if (it == g_popen.end()) return errno = EINVAL, -1;
    pid = it->second;
  }
  int retired = 0;
  CloseBionicStream(static_cast<FILE*>(stream), &retired);
  // The descriptor transaction can fail before closing the pipe. Preserve
  // the child association so the caller can retry, and do not wait on a child
  // that may still be waiting for EOF from this live pipe.
  if (!retired) return -1;
  {
    std::lock_guard<std::mutex> lock(g_popen_mutex);
    auto it = g_popen.find(stream);
    // The CRT slot may already have been reused by a new popen.
    if (it != g_popen.end() && it->second == pid) g_popen.erase(it);
  }
  int status = 0;
  int collected;
  do collected = CollectOne(pid, &status, 0); while (collected < 0 && errno == EINTR);
  if (collected < 0) return -1;
  return status;
}

// --- posix_spawn ---------------------------------------------------------------
//
// bionic's file-actions and attributes are opaque to the .so; we read the
// fields the NDK's headers define (bionic/libc/include/spawn.h).

// posix_spawn_file_actions_t and posix_spawnattr_t are opaque pointers in
// bionic (bionic/libc/include/spawn.h), so the shim owns their contents: the
// file actions below are a linked list the shim's own init/add functions build
// and DoPosixSpawn walks. Action: 0 open, 1 close, 2 dup2, 3 chdir, 4 fchdir.
struct BionicFileAction {
  BionicFileAction* next;
  int what;
  int fd;
  int new_fd;
  std::string path;
  int flags;
  uint32_t mode;
};
struct BionicFileActions {
  BionicFileAction* head;
  BionicFileAction* last;
};
struct BionicSpawnattr {
  int16_t flags;
  int pgroup;
};

void AddAction(BionicFileActions** p, BionicFileAction* a) {
  a->next = nullptr;
  if (*p == nullptr) *p = new BionicFileActions{nullptr, nullptr};
  if ((*p)->last != nullptr) {
    (*p)->last->next = a;
  } else {
    (*p)->head = a;
  }
  (*p)->last = a;
}

int BIONIC_SYSV FaInit(BionicFileActions** p) {
  *p = new BionicFileActions{nullptr, nullptr};
  return 0;
}
int BIONIC_SYSV FaDestroy(BionicFileActions** p) {
  if (p == nullptr || *p == nullptr) return 0;
  for (BionicFileAction* a = (*p)->head; a != nullptr;) {
    BionicFileAction* next = a->next;
    delete a;
    a = next;
  }
  delete *p;
  *p = nullptr;
  return 0;
}
int BIONIC_SYSV FaAddOpen(BionicFileActions** p, int fd, const char* path, int flags, uint32_t mode) {
  if (fd < 0) return EBADF;
  auto* a = new BionicFileAction{nullptr, 0, fd, fd, path != nullptr ? path : "", flags, mode};
  AddAction(p, a);
  return 0;
}
int BIONIC_SYSV FaAddClose(BionicFileActions** p, int fd) {
  if (fd < 0) return EBADF;
  auto* a = new BionicFileAction{nullptr, 1, fd, fd, "", 0, 0};
  AddAction(p, a);
  return 0;
}
int BIONIC_SYSV FaAddDup2(BionicFileActions** p, int fd, int new_fd) {
  if (fd < 0 || new_fd < 0) return EBADF;
  auto* a = new BionicFileAction{nullptr, 2, fd, new_fd, "", 0, 0};
  AddAction(p, a);
  return 0;
}
int BIONIC_SYSV FaAddChdir(BionicFileActions** p, const char* path) {
  auto* a = new BionicFileAction{nullptr, 3, -1, -1, path != nullptr ? path : "", 0, 0};
  AddAction(p, a);
  return 0;
}
int BIONIC_SYSV FaAddFchdir(BionicFileActions** p, int fd) {
  if (fd < 0) return EBADF;
  auto* a = new BionicFileAction{nullptr, 4, fd, fd, "", 0, 0};
  AddAction(p, a);
  return 0;
}

int BIONIC_SYSV AttrInit(BionicSpawnattr** p) {
  *p = new BionicSpawnattr{0, 0};
  return 0;
}
int BIONIC_SYSV AttrDestroy(BionicSpawnattr** p) {
  if (p != nullptr && *p != nullptr) {
    delete *p;
    *p = nullptr;
  }
  return 0;
}
int BIONIC_SYSV AttrSetflags(BionicSpawnattr** p, int16_t flags) {
  if (p != nullptr && *p != nullptr) (*p)->flags = flags;
  return 0;
}
int BIONIC_SYSV AttrGetflags(BionicSpawnattr** p, int16_t* flags) {
  if (flags != nullptr) *flags = (p != nullptr && *p != nullptr) ? (*p)->flags : 0;
  return 0;
}
int BIONIC_SYSV AttrStubIntArg(BionicSpawnattr**, int) { return 0; }
int BIONIC_SYSV AttrStubPtrArg(BionicSpawnattr**, void*) { return 0; }

int DoPosixSpawn(int* pid_out, const char* path, const BionicFileActions* const* actions_in,
                 const BionicSpawnattr* const*, char* const argv[], char* const envp[], bool search) {
  if (path == nullptr || argv == nullptr) return EFAULT;
  ex::SpawnRequest request;
  request.search = search;
  request.program = path;
  if (search && request.program.find_first_of("/\\") == std::string::npos &&
      request.program != "sh" && !ex::IsToolName(request.program)) {
    const char* search_path = gesher::GetEnvironment("PATH");
    if (search_path != nullptr) request.search_path = search_path;
    // Reject malformed borrowed PATH before the request becomes UTF16 argv;
    // generic command-line conversion must not replace its bytes first.
    std::wstring decoded;
    if (!gesher::DecodeEnvironmentUtf8(request.search_path, &decoded) ||
        ex::Narrow(decoded) != request.search_path) return EILSEQ;
  }
  request.argv = FromList(argv);
  if (request.argv.empty()) request.argv.push_back(path);
  std::vector<ex::ChildFd> fds;
  if (!InheritedFds(&fds, true)) return errno;
  if (actions_in != nullptr && *actions_in != nullptr) {
    for (const BionicFileAction* a = (*actions_in)->head; a != nullptr; a = a->next) {
      request.actions.push_back({a->what, a->fd, a->new_fd, a->flags, a->mode, a->path});
    }
  }
  // A private CLOEXEC event/socket/binder descriptor does not need transport
  // unless an ordered file action actually uses its original resource. This
  // avoids exporting NUL placeholders while keeping ordinary spawn available
  // to processes that hold private runtime resources.
  fds.erase(std::remove_if(fds.begin(), fds.end(), [&](const ex::ChildFd& file) {
    if (file.snapshot->kind != DescriptorKind::kUnshareable ||
        (file.snapshot->descriptor_flags & FD_CLOEXEC) == 0) return false;
    for (const ex::SpawnAction& action : request.actions) {
      if ((action.what == 2 || action.what == 4) && action.fd == file.fd) return false;
      if ((action.what == 1 && action.fd == file.fd) ||
          ((action.what == 0 || action.what == 2) && action.new_fd == file.fd)) return true;
    }
    return true;
  }), fds.end());
  for (const ex::ChildFd& file : fds) {
    request.fd_flags.emplace_back(file.fd, file.snapshot->descriptor_flags);
  }
  ex::Launch launch;
  launch.application = ex::LauncherPath();
  if (launch.application.empty()) return EILSEQ;
  ex::AppendQuoted(&launch.command_line, launch.application);
  launch.command_line += L" --spawn";
  for (const std::string& arg : ex::EncodeSpawn(request)) {
    launch.command_line += L' ';
    ex::AppendQuoted(&launch.command_line, ex::Widen(arg));
  }
  if (launch.command_line.size() + 1 > 32767) return E2BIG;
  std::vector<std::string> env;
  if (envp) env = FromList(envp);
  else if (!CurrentEnv(&env)) return errno;
  int pid = SpawnPrepared(launch, env, CurrentDir(), fds);
  if (pid < 0) return errno;  // posix_spawn returns the error number
  if (pid_out != nullptr) *pid_out = pid;
  return 0;
}

int BIONIC_SYSV PosixSpawn(int* pid, const char* path, const BionicFileActions* const* fa,
                           const BionicSpawnattr* const* attr, char* const argv[], char* const envp[]) {
  return DoPosixSpawn(pid, path, fa, attr, argv, envp, false);
}
int BIONIC_SYSV PosixSpawnp(int* pid, const char* file, const BionicFileActions* const* fa,
                            const BionicSpawnattr* const* attr, char* const argv[], char* const envp[]) {
  return DoPosixSpawn(pid, file, fa, attr, argv, envp, true);
}

// close_range(2) and closefrom(3), over the process's actual descriptor table.
constexpr unsigned kFdLimit = ofd_transfer::kDescriptorLimit;
constexpr unsigned kCloseRangeUnshare = 1u << 1, kCloseRangeCloexec = 1u << 2;

int BIONIC_SYSV CloseRange(unsigned first, unsigned last, int flags) {
  unsigned f = static_cast<unsigned>(flags);
  if ((f & ~(kCloseRangeUnshare | kCloseRangeCloexec)) != 0 || first > last) {
    return errno = EINVAL, -1;
  }
  bool cloexec = (f & kCloseRangeCloexec) != 0;
  unsigned end = std::min<unsigned>(last, kFdLimit - 1);
  // The CRT descriptor table is shared by all threads. It cannot be detached
  // for just this caller, so do not report CLOSE_RANGE_UNSHARE as implemented.
  if ((f & kCloseRangeUnshare) != 0) return errno = ENOSYS, -1;
  // Every fd in the range: one that is not open just fails with EBADF.
  for (unsigned fd = first; fd <= end; ++fd) {
    int i = static_cast<int>(fd);
    if (cloexec) {
      if (g_fcntl != nullptr) g_fcntl(i, 2 /*F_SETFD*/, 1 /*FD_CLOEXEC*/);
    } else if (g_close != nullptr) {
      g_close(i);
    }
  }
  return 0;
}

void BIONIC_SYSV Closefrom(int lowest) {
  if (lowest < 0) lowest = 0;
  CloseRange(static_cast<unsigned>(lowest), ~0u, 0);
}

// --- fork (translated arm64 code; ADR 0092) ----------------------------------------
//
// A real child process: gesher::CloneProcess (RtlCloneUserProcess) copies
// this address space copy-on-write, with only the calling thread, and both
// processes return here. Every lock a system call can take is in gesher-base's
// fork set (gesher/fork.h): AcquireForkLocks takes ntdll's loader lock, then
// all of them, then the translator's (the caller's hooks), trying each and
// starting again when one is busy, so no thread is inside any of them at the
// clone and none can be left waiting for this one. The child's copies are
// then consistent; each is made anew in the child.

struct ForkHooks {
  int (BIONIC_SYSV* try_prepare)();  // 1 with every translator lock held
  void (BIONIC_SYSV* parent)();
  void (BIONIC_SYSV* child)();
  // The translator's guest mapping lock: 0 tries it at the start of a round
  // (before the fork set; 1 when taken), 1 gives it back (a failed round, or
  // the parent after the clone), 2 makes it anew in the child.
  int (BIONIC_SYSV* mapping)(int op);
};

std::mutex g_fork_mutex;  // one fork at a time (not in the fork set: the forker's)
const ForkHooks* g_fork_hooks = nullptr;
bool TryTranslatorLocks() {
  return g_fork_hooks == nullptr || g_fork_hooks->try_prepare == nullptr ||
         g_fork_hooks->try_prepare() != 0;
}
void ReleaseTranslatorLocks() {
  if (g_fork_hooks != nullptr && g_fork_hooks->parent != nullptr) g_fork_hooks->parent();
}
bool TranslatorMapping(int op) {
  return g_fork_hooks == nullptr || g_fork_hooks->mapping == nullptr || g_fork_hooks->mapping(op) != 0;
}
// The guest mapping lock, tried for a short slice (mapping calls hold it only
// briefly; a fork under a stream of them would rarely find it free with one
// try). Not taken: the round fails like any other, and the fork set's
// deadline bounds the whole wait (gesher/fork.h).
constexpr ULONGLONG kMappingSliceMs = 50;
bool TranslatorRoundBegin() {
  const ULONGLONG start = GetTickCount64();
  for (unsigned i = 0;; ++i) {
    if (TranslatorMapping(0)) return true;
    if (GetTickCount64() - start >= kMappingSliceMs) return false;
    if (i < 64) SwitchToThread();
    else Sleep(1);
  }
}
void TranslatorRoundEnd() { TranslatorMapping(1); }

// How long a fork waits for every lock to be free at once before failing
// with EAGAIN (each is only tried; a busy one restarts the round).
constexpr unsigned kForkLockTimeoutMs = 10000;
// How long the parent waits, holding the fork set, for the child to take the
// memfd write leases it inherited (it only renews byte-range locks there).
constexpr DWORD kLeaseWaitMs = 2000;
// How long the parent waits, without the fork set, for the child to have
// started its signal mailbox thread and ptrace guard (each start is bounded
// at 5 s, gesher/fork.h).
constexpr DWORD kChildReadyMs = 15000;

// The child: none of the parent's children are its children, and none of its
// popen streams has a child of its own.
void ForkChildProcessState() {
  for (auto& kv : g_children) {
    if (kv.second.process != nullptr) CloseHandle(kv.second.process);
  }
  g_children.clear();
  g_popen.clear();
}

// Returns the child's pid in the parent, 0 in the child, -1 with errno set.
// `exit_signal`: what the child's exit sends the parent (SIGCHLD for fork).
// `run_atfork`: pthread_atfork handlers run around it (fork, vfork; not a raw
// clone system call, as on Linux).
int BIONIC_SYSV GuestFork(int exit_signal, int run_atfork, const ForkHooks* hooks) {
  if (run_atfork) RunAtforkPrepare();
  // Forked children create processes through this one (gesher/fork.h).
  gesher::StartSpawnBroker();
  gesher::PrepareFork();
  std::unique_lock<std::mutex> serial(g_fork_mutex);
  PthreadForkPrepare();
  g_fork_hooks = hooks;
  if (gesher::AcquireForkLocks(&TryTranslatorLocks, &ReleaseTranslatorLocks, kForkLockTimeoutMs,
                               &TranslatorRoundBegin, &TranslatorRoundEnd) != 0) {
    const int error = errno;
    serial.unlock();
    if (run_atfork) RunAtforkParent();
    return errno = error, -1;
  }
  // The child takes its own lease for each memfd write lease it inherits; the
  // parent waits for that before any of its threads can unmap and seal, so a
  // seal right after fork sees the child's mapping (EBUSY), as on Linux. The
  // child then waits for the parent's word: go on (`leases_go`), or be ended
  // (a parent that gave up waiting); it also goes on if the parent ends. The
  // events and the parent's handle, made before the clone, are the child's
  // too (at the same values).
  const bool leases = gesher::ForkHasMemfdWriteLeases();
  HANDLE leases_renewed = leases ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
  HANDLE leases_go = leases ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
  HANDLE parent_self = nullptr;
  if (leases) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent_self, SYNCHRONIZE,
                    FALSE, 0);
  }
  // Set by the child once its signal mailbox, its ptrace state and their
  // threads run: until then fork has not succeeded (ReinitializeAfterFork).
  HANDLE child_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (child_ready == nullptr || (leases && (leases_renewed == nullptr || leases_go == nullptr || parent_self == nullptr))) {
    for (HANDLE h : {child_ready, leases_renewed, leases_go, parent_self}) {
      if (h != nullptr) CloseHandle(h);
    }
    gesher::ReleaseForkLocks(false);
    TranslatorMapping(1);
    serial.unlock();
    if (run_atfork) RunAtforkParent();
    return errno = EAGAIN, -1;
  }
  HANDLE process = nullptr;
  DWORD pid = 0;
  const int cloned = gesher::CloneProcess(&process, &pid);
  const int error = errno;
  if (cloned == 0) {
    // The child. Its copies of the locks are consistent (no thread was in
    // them) and its only thread holds them: drop the parent's other threads'
    // state, then make the locks anew.
    if (hooks != nullptr && hooks->child != nullptr) hooks->child();
    TranslatorMapping(2);
    PthreadForkChild();
    SignalForkChild();
    ForkChildProcessState();
    PtraceForkChild();
    gesher::ReleaseForkLocks(true);  // renews the inherited memfd leases
    if (leases) {
      SetEvent(leases_renewed);
      // No guest code runs before the parent decided: a parent that gave up
      // waiting ends this process instead of saying go.
      HANDLE waits[2] = {leases_go, parent_self};
      WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      for (HANDLE h : {leases_renewed, leases_go, parent_self}) CloseHandle(h);
    }
    serial.release();
    new (&g_fork_mutex) std::mutex();
    if (!gesher::ReinitializeAfterFork()) {
      // It could not watch its own signals (or be traced): not a child the
      // parent may return. The parent sees it end without being ready.
      TerminateProcess(GetCurrentProcess(), 0x7f);
    }
    SetEvent(child_ready);
    CloseHandle(child_ready);
    if (run_atfork) RunAtforkChild();
    return 0;
  }
  // Whether the child took its leases. The wait is bounded and short (this
  // thread holds the whole fork set meanwhile, so every other thread that
  // makes a system call waits too): the child only renews locks there.
  DWORD lease_wait = WAIT_OBJECT_0;
  if (leases) {
    if (cloned > 0) {
      HANDLE waits[2] = {leases_renewed, process};
      lease_wait = WaitForMultipleObjects(2, waits, FALSE, kLeaseWaitMs);
      // Only a child that took them goes on: one that did not is ended below
      // (it waits for this word, so it has run no guest code either way).
      if (lease_wait == WAIT_OBJECT_0) SetEvent(leases_go);
    }
    for (HANDLE h : {leases_renewed, leases_go, parent_self}) CloseHandle(h);
  }
  const bool leases_failed = cloned > 0 && lease_wait != WAIT_OBJECT_0;
  if (leases_failed) {
    // The child did not take its leases in time (or ended first): returning
    // its pid would let a seal succeed while the child maps the memfd, which
    // Linux never allows. It has run no guest code (it waits for the word
    // above), so it is ended and fork fails as fork(2) may.
    TerminateProcess(process, 0);
    WaitForSingleObject(process, kLeaseWaitMs);
    CloseHandle(process);
  }
  gesher::ReleaseForkLocks(false);
  TranslatorMapping(1);
  serial.unlock();
  // The child's start: its mailbox thread and ptrace guard (outside the fork
  // set: they start threads). Not ready in time, or ended first: it has run
  // no guest code (it signals before fork returns in it), so it is ended and
  // fork fails, rather than return a child that loses signals.
  DWORD ready_wait = WAIT_OBJECT_0;
  if (cloned > 0 && !leases_failed) {
    HANDLE waits[2] = {child_ready, process};
    ready_wait = WaitForMultipleObjects(2, waits, FALSE, kChildReadyMs);
    if (ready_wait != WAIT_OBJECT_0) {
      TerminateProcess(process, 0);
      WaitForSingleObject(process, kLeaseWaitMs);
      CloseHandle(process);
    }
  }
  CloseHandle(child_ready);
  if (cloned > 0 && !leases_failed && ready_wait != WAIT_OBJECT_0) {
    gesher::ForgetChildMailbox(pid);
    gesher::logging::Record(0 /*LOG_ID_MAIN*/, 5 /*ANDROID_LOG_WARN*/, "gesher-fork",
                            "fork: the child could not start its signal and ptrace threads; EAGAIN");
    if (run_atfork) RunAtforkParent();
    return errno = EAGAIN, -1;
  }
  if (leases_failed) {
    // Its mailbox and broker registration (their locks are in the fork set).
    gesher::ForgetChildMailbox(pid);
    // Logged only now: the log's locks are in the fork set.
    gesher::logging::Record(0 /*LOG_ID_MAIN*/, 5 /*ANDROID_LOG_WARN*/, "gesher-fork",
                            lease_wait == WAIT_TIMEOUT
                                ? "fork: the child did not take its memfd write leases in time; EAGAIN"
                                : "fork: the child ended before it took its memfd write leases; EAGAIN");
    if (run_atfork) RunAtforkParent();
    return errno = EAGAIN, -1;
  }
  if (cloned > 0) Track(static_cast<int>(pid), process, exit_signal, true);
  if (run_atfork) RunAtforkParent();
  if (cloned < 0) return errno = error, -1;
  return static_cast<int>(pid);
}

unsigned BIONIC_SYSV SignalExitCodeForTranslator(int sig) { return gesher::SignalExitCode(sig); }

// The raw exit system call (arm64's 93): it ends the calling thread, and
// with the last thread the process, with that status. The program's threads
// are those it made; a forked child and gesher-exec's program have no others
// (gesher-base's own helper threads are not the program's). Elsewhere (ART's
// process) the process ends only when Windows reports the last thread. As on
// Linux, nothing is flushed.
[[noreturn]] void BIONIC_SYSV ExitThreadRaw(int code) {
  ULONG last = 0;
  using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static auto query = reinterpret_cast<QueryThreadFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
  // ThreadAmILastThread (12).
  if (query != nullptr) query(GetCurrentThread(), 12, &last, sizeof(last), nullptr);
  if (last != 0 || ((gesher::InForkedChild() || SignalsAuthoritative()) && OnlyProgramThread())) {
    if (gesher::InForkedChild()) TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code) & 0xff);
    ExitProcess(static_cast<UINT>(code) & 0xff);
  }
  ExitThread(static_cast<DWORD>(code) & 0xff);
}

}  // namespace

void ChildWaitStateChanged() {
  g_child_wait_generation.fetch_add(1, std::memory_order_release);
  NotifyInterruptibleWaiters();
}

int KillOtherProcess(int pid, int sig) {
  // A child that already exited is a zombie: it exists (for any signal, a no-op)
  // until waitpid collects it. A live process -- a child of the shim or any
  // other Gesher process -- is signalled through gesher-base's mailbox, which
  // delivers it to the target's handlers (the bionic shim's hook there),
  // terminates on a default-terminating signal, and checks existence for 0.
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(pid);
    if (it != g_children.end() &&
        (it->second.exited || WaitForSingleObject(it->second.process, 0) == WAIT_OBJECT_0)) {
      return 0;
    }
  }
  return gesher_kill_other(pid, sig);
}

void RegisterProcess(SymbolTable& t) {
  // Resolve directly from the table to avoid re-entering its initialization.
  auto original = [&t](const char* name) -> void* {
    auto it = t.find(name);
    return it != t.end() ? it->second : nullptr;
  };
  g_close = reinterpret_cast<CloseFn>(original("close"));
  g_fcntl = reinterpret_cast<FcntlFn>(original("fcntl"));
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("fork", ForkUnsupported);
  E("vfork", ForkUnsupported);
  E("daemon", DaemonUnsupported);
  E("sbrk", Sbrk);
  E("brk", Brk);
  // For gesher_arm64.dll's guest fork/vfork/clone (ADR 0092); x86-64 code's
  // fork stays unsupported (ADR 0055).
  E("__gesher_fork", GuestFork);
  E("__gesher_exit_thread", ExitThreadRaw);
  E("__gesher_signal_exit_code", SignalExitCodeForTranslator);
  E("execve", Execve);
  E("execv", Execv);
  E("execvp", Execvp);
  E("execvpe", Execvpe);
  E("execl", gesher_bionic_execl);
  E("execlp", gesher_bionic_execlp);
  E("execle", gesher_bionic_execle);
  // The va_list forms, for the arm64 translator (guest_libc.cc).
  E("__gesher_vexecl", gesher_bionic_vexecl);
  E("__gesher_vexeclp", gesher_bionic_vexeclp);
  E("__gesher_vexecle", gesher_bionic_vexecle);
  E("waitpid", Waitpid);
  E("wait", Wait);
  E("wait4", Wait4);
  E("system", System);
  E("popen", Popen);
  E("pclose", Pclose);
  E("posix_spawn", PosixSpawn);
  E("posix_spawnp", PosixSpawnp);
  E("posix_spawn_file_actions_init", FaInit);
  E("posix_spawn_file_actions_destroy", FaDestroy);
  E("posix_spawn_file_actions_addopen", FaAddOpen);
  E("posix_spawn_file_actions_addclose", FaAddClose);
  E("posix_spawn_file_actions_adddup2", FaAddDup2);
  E("posix_spawn_file_actions_addchdir_np", FaAddChdir);
  E("posix_spawn_file_actions_addfchdir_np", FaAddFchdir);
  E("posix_spawnattr_init", AttrInit);
  E("posix_spawnattr_destroy", AttrDestroy);
  E("posix_spawnattr_setflags", AttrSetflags);
  E("posix_spawnattr_getflags", AttrGetflags);
  E("posix_spawnattr_setpgroup", AttrStubIntArg);
  E("posix_spawnattr_setsigmask", AttrStubPtrArg);
  E("posix_spawnattr_setsigmask64", AttrStubPtrArg);
  E("posix_spawnattr_setsigdefault", AttrStubPtrArg);
  E("posix_spawnattr_setsigdefault64", AttrStubPtrArg);
  E("posix_spawnattr_setschedparam", AttrStubPtrArg);
  E("posix_spawnattr_setschedpolicy", AttrStubIntArg);
  E("close_range", CloseRange);
  E("closefrom", Closefrom);
#undef E
  // Android's CA directory names over every path-taking entry.
  WrapSystemCacerts(t);
}

}  // namespace gesher::bionic

// fork (gesher/fork.h): the children and popen tables in a forked child.
GESHER_FORK_LOCK(gesher::kForkOrderProcess, std::mutex, gesher::bionic::g_children_mutex);
GESHER_FORK_LOCK(gesher::kForkOrderProcess, std::mutex, gesher::bionic::g_popen_mutex);
