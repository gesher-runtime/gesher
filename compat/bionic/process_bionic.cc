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

#include <gesher/bionic_shim.h>
#include <gesher/environment.h>
#include <gesher/environment_utf16.h>

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
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

// --- the shim's children -------------------------------------------------------

struct Child {
  HANDLE process = nullptr;
  bool exited = false;
  int status = 0;  // Linux waitpid status word, once reaped by the watcher
  HANDLE wait = nullptr;

};

std::mutex g_children_mutex;
std::map<int, Child> g_children;  // pid -> child
std::atomic<uint64_t> g_children_generation{0};

void CALLBACK OnChildExit(void* param, BOOLEAN) {
  int pid = static_cast<int>(reinterpret_cast<intptr_t>(param));
  int sig_status = 0;
  bool deliver = false;
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(pid);
    if (it != g_children.end() && !it->second.exited) {
      DWORD code = 0;
      GetExitCodeProcess(it->second.process, &code);
      it->second.status = ex::WaitStatus(code);
      it->second.exited = true;
      sig_status = it->second.status;
      deliver = true;
      g_children_generation.fetch_add(1, std::memory_order_release);
    }
  }
  if (deliver) {
    NotifyInterruptibleWaiters();
    DeliverExternalSignal(kSigchld, 0, kCldExited, sig_status, false);
  }
}

void Track(int pid, HANDLE process) {
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    Child& c = g_children[pid];
    c.process = process;
    c.exited = false;
    c.status = 0;
    c.wait = nullptr;
    if (!RegisterWaitForSingleObject(&c.wait, process, OnChildExit,
                                    reinterpret_cast<void*>(static_cast<intptr_t>(pid)), INFINITE,
                                    WT_EXECUTEONLYONCE)) c.wait = nullptr;
    g_children_generation.fetch_add(1, std::memory_order_release);
  }
  // A failed watcher registration must also wake an existing any-child
  // waiter so it can use the explicitly bounded fallback below.
  NotifyInterruptibleWaiters();
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
                   const std::string& dir, const std::vector<ex::ChildFd>& fds) {
  std::wstring environment = ex::EnvironmentBlock(env);
  if (environment.empty()) return -1;
  std::wstring cwd = dir.empty() ? L"" : ex::WindowsPath(dir);
  PROCESS_INFORMATION pi = {};
  DWORD werr = ex::SpawnWithFds(launch, environment.c_str(), cwd, fds, &pi);
  if (werr != 0) return errno = ex::ErrnoFromWin32(werr), -1;
  CloseHandle(pi.hThread);
  int pid = static_cast<int>(pi.dwProcessId);
  Track(pid, pi.hProcess);
  return pid;
}

int SpawnProgram(const std::string& program, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, const std::string& dir, bool search,
                 const std::vector<ex::ChildFd>& fds) {
  ex::Launch launch;
  int e = ex::PrepareLaunchProgram(program, argv, dir, search, &launch);
  if (e != 0) return errno = e, -1;
  return SpawnPrepared(launch, env, dir, fds);
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

// --- execve / execvp and friends ----------------------------------------------

// Returns only on failure (errno set); on success it exits the process.
void ExecReplace(const std::string& program, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, bool search) {
  std::vector<ex::ChildFd> fds;
  if (!InheritedFds(&fds)) return;
  int pid = SpawnProgram(program, argv, env, CurrentDir(), search, fds);
  fds.clear();  // snapshots must not keep child pipe endpoints open while waiting
  if (pid < 0) return;
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
    std::fflush(nullptr);
    ExitProcess(static_cast<UINT>(ex::ExitStatus(code)) & 0xff);
  }
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

// --- waitpid / wait / wait4 ----------------------------------------------------

constexpr int kWNOHANG = 1;

int CollectOne(int pid, int* status, int options) {
  for (;;) {
    uint64_t observed;
    HANDLE retired_wait = nullptr, retired_process = nullptr;
    int got = 0, sig_status = 0;
    bool fallback = false, deliver = false;
    {
      std::lock_guard<std::mutex> lock(g_children_mutex);
      if (g_children.empty()) return errno = 10 /*ECHILD*/, -1;
      // A specific pid, or any (-1/0).
      bool found = false;
      for (auto& kv : g_children) {
        if (pid > 0 && kv.first != pid) continue;
        found = true;
        if (!kv.second.exited && kv.second.wait == nullptr) {
          // Registration failure cannot strand an already spawned child.
          // Only this fallback probes native state; successfully watched
          // children wait indefinitely on actual lifecycle notifications.
          DWORD ready = WaitForSingleObject(kv.second.process, 0);
          if (ready == WAIT_FAILED) return errno = ex::ErrnoFromWin32(GetLastError()), -1;
          if (ready == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (!GetExitCodeProcess(kv.second.process, &code)) {
              return errno = ex::ErrnoFromWin32(GetLastError()), -1;
            }
            kv.second.status = ex::WaitStatus(code);
            kv.second.exited = true;
            sig_status = kv.second.status;
            deliver = true;
          } else {
            fallback = true;
          }
        }
        if (kv.second.exited) {
          if (status != nullptr) *status = kv.second.status;
          got = kv.first;
          retired_wait = kv.second.wait;
          retired_process = kv.second.process;
          g_children.erase(kv.first);
          g_children_generation.fetch_add(1, std::memory_order_release);
          break;
        }
        if (pid > 0) break;
      }
      // None exited: wait below with the lock released.
      if (!found) return errno = 10 /*ECHILD*/, -1;  // no such child
      observed = g_children_generation.load(std::memory_order_relaxed);
    }
    if (got != 0) {
      // Other waiters must observe removal even if one of them missed the
      // earlier exit notification. No callback/handle retirement runs under
      // the child registry lock.
      NotifyInterruptibleWaiters();
      if (retired_wait != nullptr) UnregisterWaitEx(retired_wait, nullptr);
      if (retired_process != nullptr) CloseHandle(retired_process);
      if (deliver) DeliverExternalSignal(kSigchld, 0, kCldExited, sig_status, false);
      return got;
    }
    if ((options & kWNOHANG) != 0) return 0;
    // Generation is captured with the eligibility scan. A change before
    // arming is observed by the predicate; a later change wakes the condition.
    int error = 0;
    if (fallback) InterruptibleSleep(20, nullptr, &error);
    else InterruptibleWaitForChange(g_children_generation, observed, &error);
    if (error != 0) return errno = error, -1;
  }
}

int BIONIC_SYSV Waitpid(int pid, int* status, int options) { return CollectOne(pid, status, options); }
int BIONIC_SYSV Wait(int* status) { return CollectOne(-1, status, 0); }
int BIONIC_SYSV Wait4(int pid, int* status, int options, void* /*rusage*/) {
  return CollectOne(pid, status, options);
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
  if (CollectOne(pid, &status, 0) < 0) return -1;
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
  if (CollectOne(pid, &status, 0) < 0) return -1;
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

}  // namespace

int KillOtherProcess(int pid, int sig) {
  // A child that already exited is a zombie: it exists (for any signal, a no-op)
  // until waitpid collects it. A live process -- a child of the shim or any
  // other Gesher process -- is signalled through gesher-base's mailbox, which
  // delivers it to the target's handlers (the bionic shim's hook there),
  // terminates on a default-terminating signal, and checks existence for 0.
  {
    std::lock_guard<std::mutex> lock(g_children_mutex);
    auto it = g_children.find(pid);
    if (it != g_children.end() && it->second.exited) return 0;
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
  E("execve", Execve);
  E("execv", Execv);
  E("execvp", Execvp);
  E("execvpe", Execvpe);
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
