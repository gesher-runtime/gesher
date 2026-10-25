// gesher bionic shim: creating processes from native code (docs/exec.md,
// docs/decisions/0055-exec2-native-processes.md).
//
// Windows has no fork(), and an address space cannot be copied faithfully for
// ART's heap and threads (ADR 0035). What apps' native code really does is
// start another program: system(), popen(), posix_spawn(), or fork()+exec*().
// This file provides them over the launcher's program finder (exec_spawn.h) and
// CreateProcessW:
//
//  - system(), popen()/pclose(): a child running the launcher's shell (sh -c);
//    posix_spawn()/posix_spawnp(): the program with the file actions and the
//    attributes applied. None of these needs fork.
//  - fork()/vfork(): a vfork-style child that runs inline on the calling
//    thread (fork returns 0). The fd, environment and working-directory calls
//    it makes are recorded, not applied to this process; its exec*() spawns the
//    real program with them and fork() returns the child's pid to the "parent";
//    its exit()/_exit() ends the child without spawning. Anything else the
//    child does between fork and exec (real work, touching the parent's memory)
//    is not supported, as vfork never allowed it.
//  - wait()/waitpid()/wait4(): over the shim's own children, with Linux's
//    status word (WIFEXITED/WIFSIGNALED). A child's exit raises SIGCHLD.
//  - execv*(): outside a fork child, where Linux replaces the image, the shim
//    spawns the program inheriting this process's fds, environment and working
//    directory, waits, and exits with its status (the pid changes; documented).
//
// exec_spawn.h finds the program and decides how it runs (an ELF executable in
// gesher-exec, a PE as itself, sh/scripts/tools in the launcher).

#include "shim_internal.h"

#include <gesher/bionic_shim.h>

#include <windows.h>

#include <errno.h>
#include <io.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "exec_spawn.h"

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
    }
  }
  if (deliver) DeliverExternalSignal(kSigchld, 0, kCldExited, sig_status, false);
}

void Track(int pid, HANDLE process) {
  std::lock_guard<std::mutex> lock(g_children_mutex);
  Child& c = g_children[pid];
  c.process = process;
  c.exited = false;
  c.status = 0;
  RegisterWaitForSingleObject(&c.wait, process, OnChildExit,
                              reinterpret_cast<void*>(static_cast<intptr_t>(pid)), INFINITE,
                              WT_EXECUTEONLYONCE);
}

// --- the environment and working directory as UTF-8 ----------------------------

std::vector<std::string> CurrentEnv() {
  std::vector<std::string> out;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* e = block; *e != 0; e += wcslen(e) + 1) {
      if (*e != L'=') out.push_back(ex::Narrow(e));
    }
    FreeEnvironmentStringsW(block);
  }
  return out;
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

// --- the vfork-style child -----------------------------------------------------
//
// A fork child records fd, environment and directory changes without touching
// this process; exec materialises them. fd numbers are handed out from a shadow
// set that starts as the parent's open fds.

struct FdAction {
  enum Kind { kDup2, kClose, kOpen } kind;
  int a = -1, b = -1;        // dup2(a->b); close(a)
  std::string path;          // open
  int flags = 0, mode = 0;
};

// __builtin_setjmp/longjmp (a plain register save/restore, no SEH unwinding,
// unlike the UCRT's longjmp): fork() returns 0 and its frame is abandoned as
// vfork's is; the child runs deeper on the stack and exec/exit longjmps back to
// fork's buffer (kept in this heap object). The 5-word buffer the builtins use.
struct ForkChild {
  void* resume[5];                      // __builtin_setjmp buffer; fork() returns here
  std::map<int, bool> present;          // fd -> open, the shadow table
  std::vector<FdAction> fd_actions;
  std::vector<std::string> env;         // the child's environment, editable
  std::string dir;                      // the child's working directory
  int pid = 0;                          // the spawned pid, for fork()'s return
  bool exited = false;                  // the child called _exit without exec
  int exit_status = 0;                  // its status word
};

thread_local ForkChild* t_fork = nullptr;
// The child being returned through the longjmp; read after __builtin_setjmp,
// so the fork() local is not relied on across the jump (it may be clobbered).
thread_local ForkChild* t_fork_result = nullptr;

int LowestFreeFd(ForkChild* c) {
  int fd = 0;
  while (c->present.count(fd) && c->present[fd]) ++fd;
  return fd;
}

// --- the originals RegisterProcess wraps --------------------------------------

using OpenFn = int (BIONIC_SYSV*)(const char*, int, int);
using OpenatFn = int (BIONIC_SYSV*)(int, const char*, int, int);
using CloseFn = int (BIONIC_SYSV*)(int);
using Dup2Fn = int (BIONIC_SYSV*)(int, int);
using ChdirFn = int (BIONIC_SYSV*)(const char*);
using SetenvFn = int (BIONIC_SYSV*)(const char*, const char*, int);
using UnsetenvFn = int (BIONIC_SYSV*)(const char*);

OpenFn g_open = nullptr;
OpenatFn g_openat = nullptr;
CloseFn g_close = nullptr;
Dup2Fn g_dup2 = nullptr;
ChdirFn g_chdir = nullptr;
SetenvFn g_setenv = nullptr;
UnsetenvFn g_unsetenv = nullptr;

// --- spawning ------------------------------------------------------------------

// Builds the child fd set, environment and cwd, spawns `argv[0]`, tracks the
// pid. `search`: look argv[0] up in PATH (execvp/posix_spawnp). Returns the
// pid, or -1 with errno set.
int SpawnProgram(const std::string& program, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, const std::string& dir, bool search,
                 const std::vector<ex::ChildFd>& fds) {
  ex::Launch launch;
  int e = ex::PrepareLaunchProgram(program, argv, dir, search, &launch);
  if (e != 0) return errno = e, -1;
  std::wstring environment = ex::EnvironmentBlock(env);
  std::wstring cwd = dir.empty() ? L"" : ex::WindowsPath(dir);
  PROCESS_INFORMATION pi = {};
  DWORD werr = ex::SpawnWithFds(launch, environment.c_str(), cwd, fds, &pi);
  if (werr != 0) return errno = ex::ErrnoFromWin32(werr), -1;
  CloseHandle(pi.hThread);
  int pid = static_cast<int>(pi.dwProcessId);
  Track(pid, pi.hProcess);
  return pid;
}

// The parent's currently open fds and their handles.
std::vector<ex::ChildFd> InheritedFds() {
  std::vector<ex::ChildFd> out;
  for (int fd = 0; fd < 2048; ++fd) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h != INVALID_HANDLE_VALUE && h != nullptr) out.push_back({fd, h});
  }
  return out;
}

// --- fork / vfork --------------------------------------------------------------

int BIONIC_SYSV Fork() {
  auto* child = new ForkChild();
  child->env = CurrentEnv();
  child->dir = CurrentDir();
  for (const ex::ChildFd& f : InheritedFds()) child->present[f.fd] = true;
  if (__builtin_setjmp(child->resume) != 0) {
    // Back from the child's exec() or _exit(); re-read the child (the local may
    // not survive the jump).
    ForkChild* child = t_fork_result;
    int result;
    if (child->pid > 0) {
      result = child->pid;
    } else if (child->exited) {
      // The child exited without exec: record it so waitpid can collect it.
      int pid = static_cast<int>(GetCurrentProcessId()) ^ 0x40000000;  // a synthetic pid
      static std::atomic<int> counter{1};
      pid = 0x40000000 | (counter.fetch_add(1) & 0x3fffffff);
      std::lock_guard<std::mutex> lock(g_children_mutex);
      Child& c = g_children[pid];
      c.process = nullptr;
      c.exited = true;
      c.status = child->exit_status;
      result = pid;
      DeliverExternalSignal(kSigchld, 0, kCldExited, child->exit_status, false);
    } else {
      result = -1;  // the child failed to start a program
      errno = child->pid == -2 ? ENOEXEC : EAGAIN;
    }
    delete child;
    return result;
  }
  t_fork = child;
  return 0;  // the child
}

// Materialises the child's recorded fd/env/dir changes and spawns the program;
// its locals are destroyed before the longjmp in ChildExec. Returns the pid, or
// -1 (errno set).
int MaterializeAndSpawn(ForkChild* child, const std::string& program,
                        const std::vector<std::string>& argv, bool search) {
  std::map<int, HANDLE> handles;
  for (const ex::ChildFd& f : InheritedFds()) handles[f.fd] = f.handle;
  std::vector<int> opened;  // real parent fds opened to materialise, closed after
  for (const FdAction& a : child->fd_actions) {
    switch (a.kind) {
      case FdAction::kClose:
        handles.erase(a.a);
        break;
      case FdAction::kDup2: {
        auto it = handles.find(a.a);
        if (it != handles.end()) handles[a.b] = it->second;
        break;
      }
      case FdAction::kOpen: {
        int fd = g_open != nullptr ? g_open(a.path.c_str(), a.flags, a.mode) : -1;
        if (fd >= 0) {
          opened.push_back(fd);
          handles[a.a] = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        } else {
          handles.erase(a.a);
        }
        break;
      }
    }
  }
  std::vector<ex::ChildFd> fds;
  for (auto& kv : handles) {
    if (kv.second != INVALID_HANDLE_VALUE && kv.second != nullptr) fds.push_back({kv.first, kv.second});
  }
  int pid = SpawnProgram(program, argv, child->env, child->dir, search, fds);
  for (int fd : opened) {
    if (g_close != nullptr) g_close(fd);
  }
  return pid;
}

// The child's exec*(): spawn with its recorded state, then return to fork()
// with the pid (or an error). No locals survive the longjmp.
[[noreturn]] void ChildExec(const std::string& program, const std::vector<std::string>& argv,
                            bool search) {
  ForkChild* child = t_fork;
  t_fork = nullptr;
  int pid = MaterializeAndSpawn(child, program, argv, search);
  child->pid = pid > 0 ? pid : (errno == ENOEXEC ? -2 : -1);
  t_fork_result = child;
  __builtin_longjmp(child->resume, 1);
}

// --- execve / execvp and friends ----------------------------------------------

// Returns only on failure (errno set); on success it exits the process.
void ExecReplace(const std::string& program, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, bool search) {
  // No fork child: Linux would replace this image. The shim spawns the program
  // inheriting this process's fds/env/cwd, waits, and exits with its status.
  int pid = SpawnProgram(program, argv, env, CurrentDir(), search, InheritedFds());
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
  if (t_fork != nullptr) ChildExec(path, args, search);  // does not return
  std::vector<std::string> env = envp != nullptr ? FromList(envp) : CurrentEnv();
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
    HANDLE h = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_children_mutex);
      if (g_children.empty()) return errno = 10 /*ECHILD*/, -1;
      // A specific pid, or any (-1/0).
      for (auto& kv : g_children) {
        if (pid > 0 && kv.first != pid) continue;
        if (kv.second.exited) {
          if (status != nullptr) *status = kv.second.status;
          int got = kv.first;
          if (kv.second.wait != nullptr) UnregisterWaitEx(kv.second.wait, nullptr);
          if (kv.second.process != nullptr) CloseHandle(kv.second.process);
          g_children.erase(kv.first);
          return got;
        }
        if (pid > 0) {
          h = kv.second.process;
          break;
        }
      }
      if (pid <= 0) {
        // None exited yet; wait on them all below (release the lock first).
      } else if (h == nullptr) {
        return errno = 10 /*ECHILD*/, -1;  // no such child
      }
    }
    if ((options & kWNOHANG) != 0) return 0;
    // Block until a child exits (the watcher sets `exited`), then re-scan.
    InterruptibleSleep(20, nullptr);
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
  int pid = SpawnProgram("/system/bin/sh", argv, CurrentEnv(), CurrentDir(), false, InheritedFds());
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
  SetHandleInformation(parent_end, HANDLE_FLAG_INHERIT, 0);
  std::vector<ex::ChildFd> fds = InheritedFds();
  // Replace the child's stdout (read) or stdin (write) with the pipe.
  int target = read ? 1 : 0;
  for (auto& f : fds) {
    if (f.fd == target) f.handle = child_end;
  }
  bool found = false;
  for (auto& f : fds) found = found || f.fd == target;
  if (!found) fds.push_back({target, child_end});
  std::vector<std::string> argv{"/system/bin/sh", "-c", command};
  int pid = SpawnProgram("/system/bin/sh", argv, CurrentEnv(), CurrentDir(), false, fds);
  CloseHandle(child_end);
  if (pid < 0) {
    CloseHandle(parent_end);
    return nullptr;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(parent_end),
                           (read ? _O_RDONLY : _O_WRONLY) | _O_BINARY | _O_NOINHERIT);
  if (fd < 0) {
    CloseHandle(parent_end);
    return nullptr;
  }
  FILE* f = _fdopen(fd, read ? "rb" : "wb");
  if (f == nullptr) {
    _close(fd);
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
    g_popen.erase(it);
  }
  std::fclose(static_cast<FILE*>(stream));
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
  auto* a = new BionicFileAction{nullptr, 0, fd, fd, path != nullptr ? path : "", flags, mode};
  AddAction(p, a);
  return 0;
}
int BIONIC_SYSV FaAddClose(BionicFileActions** p, int fd) {
  auto* a = new BionicFileAction{nullptr, 1, fd, fd, "", 0, 0};
  AddAction(p, a);
  return 0;
}
int BIONIC_SYSV FaAddDup2(BionicFileActions** p, int fd, int new_fd) {
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
  std::map<int, HANDLE> handles;
  for (const ex::ChildFd& f : InheritedFds()) handles[f.fd] = f.handle;
  std::vector<int> opened;
  std::string dir = CurrentDir();
  if (actions_in != nullptr && *actions_in != nullptr) {
    for (const BionicFileAction* a = (*actions_in)->head; a != nullptr; a = a->next) {
      switch (a->what) {
        case 0: {  // open
          int fd = g_open != nullptr ? g_open(a->path.c_str(), a->flags, static_cast<int>(a->mode)) : -1;
          if (fd >= 0) {
            opened.push_back(fd);
            handles[a->new_fd] = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
          } else {
            handles.erase(a->new_fd);
          }
          break;
        }
        case 1:  // close
          handles.erase(a->fd);
          break;
        case 2: {  // dup2
          auto it = handles.find(a->fd);
          if (it != handles.end()) handles[a->new_fd] = it->second;
          break;
        }
        case 3:  // chdir
          dir = a->path;
          break;
        default:
          break;
      }
    }
  }
  std::vector<ex::ChildFd> fds;
  for (auto& kv : handles) {
    if (kv.second != INVALID_HANDLE_VALUE && kv.second != nullptr) fds.push_back({kv.first, kv.second});
  }
  std::vector<std::string> args = FromList(argv);
  if (args.empty()) args.push_back(path);
  std::vector<std::string> env = envp != nullptr ? FromList(envp) : CurrentEnv();
  int pid = SpawnProgram(path, args, env, dir, search, fds);
  for (int fd : opened) {
    if (g_close != nullptr) g_close(fd);
  }
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

// --- fd / env / dir calls, child-aware -----------------------------------------

int BIONIC_SYSV Open(const char* path, int flags, int mode) {
  if (t_fork != nullptr) {
    ForkChild* c = t_fork;
    int fd = LowestFreeFd(c);
    c->present[fd] = true;
    FdAction a;
    a.kind = FdAction::kOpen;
    a.a = fd;
    a.path = path != nullptr ? path : "";
    a.flags = flags;
    a.mode = mode;
    c->fd_actions.push_back(a);
    return fd;
  }
  return g_open != nullptr ? g_open(path, flags, mode) : (errno = ENOSYS, -1);
}
int BIONIC_SYSV Openat(int dirfd, const char* path, int flags, int mode) {
  if (t_fork != nullptr && path != nullptr && (path[0] == '/' || path[0] == '\\' || (path[0] != 0 && path[1] == ':'))) {
    return Open(path, flags, mode);  // an absolute path: the dirfd is moot
  }
  return g_openat != nullptr ? g_openat(dirfd, path, flags, mode) : (errno = ENOSYS, -1);
}
int BIONIC_SYSV Close(int fd) {
  if (t_fork != nullptr) {
    ForkChild* c = t_fork;
    c->present[fd] = false;
    FdAction a;
    a.kind = FdAction::kClose;
    a.a = fd;
    c->fd_actions.push_back(a);
    return 0;
  }
  return g_close != nullptr ? g_close(fd) : (errno = EBADF, -1);
}
int BIONIC_SYSV Dup2(int old_fd, int new_fd) {
  if (t_fork != nullptr) {
    ForkChild* c = t_fork;
    if (!c->present.count(old_fd) || !c->present[old_fd]) return errno = EBADF, -1;
    c->present[new_fd] = true;
    FdAction a;
    a.kind = FdAction::kDup2;
    a.a = old_fd;
    a.b = new_fd;
    c->fd_actions.push_back(a);
    return new_fd;
  }
  return g_dup2 != nullptr ? g_dup2(old_fd, new_fd) : (errno = EBADF, -1);
}
int BIONIC_SYSV Dup3(int old_fd, int new_fd, int /*flags*/) { return Dup2(old_fd, new_fd); }

int BIONIC_SYSV Chdir(const char* path) {
  if (t_fork != nullptr) {
    t_fork->dir = path != nullptr ? path : "";
    return 0;
  }
  return g_chdir != nullptr ? g_chdir(path) : (errno = ENOSYS, -1);
}

void EditEnv(std::vector<std::string>* env, const std::string& name, const char* value) {
  for (auto it = env->begin(); it != env->end(); ++it) {
    if (it->size() > name.size() && (*it)[name.size()] == '=' && it->compare(0, name.size(), name) == 0) {
      if (value == nullptr) {
        env->erase(it);
      } else {
        *it = name + "=" + value;
      }
      return;
    }
  }
  if (value != nullptr) env->push_back(name + "=" + value);
}
int BIONIC_SYSV Setenv(const char* name, const char* value, int overwrite) {
  if (name == nullptr || value == nullptr) return errno = EINVAL, -1;
  if (t_fork != nullptr) {
    std::string n = name;
    bool exists = false;
    for (const std::string& e : t_fork->env) {
      if (e.size() > n.size() && e[n.size()] == '=' && e.compare(0, n.size(), n) == 0) exists = true;
    }
    if (overwrite || !exists) EditEnv(&t_fork->env, n, value);
    return 0;
  }
  return g_setenv != nullptr ? g_setenv(name, value, overwrite) : (errno = ENOSYS, -1);
}
int BIONIC_SYSV Unsetenv(const char* name) {
  if (name == nullptr) return errno = EINVAL, -1;
  if (t_fork != nullptr) {
    EditEnv(&t_fork->env, name, nullptr);
    return 0;
  }
  return g_unsetenv != nullptr ? g_unsetenv(name) : (errno = ENOSYS, -1);
}
int BIONIC_SYSV Putenv(char* assignment) {
  if (assignment == nullptr) return errno = EINVAL, -1;
  const char* eq = strchr(assignment, '=');
  if (eq == nullptr) return Unsetenv(assignment);
  return Setenv(std::string(assignment, eq - assignment).c_str(), eq + 1, 1);
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

void ExitIfVforkChild(int status) {
  if (t_fork != nullptr) {
    ForkChild* child = t_fork;
    t_fork = nullptr;
    child->exited = true;
    child->exit_status = (status & 0xff) << 8;  // WIFEXITED
    child->pid = 0;
    t_fork_result = child;
    __builtin_longjmp(child->resume, 1);
  }
}

void RegisterProcess(SymbolTable& t) {
  // The originals the child-aware wrappers below delegate to, taken from the
  // table the earlier Register* filled (RegisterProcess runs last). Looked up in
  // `t` directly, not through LookupBionicSymbol, which would re-enter the
  // table's own initialization.
  auto original = [&t](const char* name) -> void* {
    auto it = t.find(name);
    return it != t.end() ? it->second : nullptr;
  };
#define GET(var, name, type) var = reinterpret_cast<type>(original(name))
  GET(g_open, "open", OpenFn);
  GET(g_openat, "openat", OpenatFn);
  GET(g_close, "close", CloseFn);
  GET(g_dup2, "dup2", Dup2Fn);
  GET(g_chdir, "chdir", ChdirFn);
  GET(g_setenv, "setenv", SetenvFn);
  GET(g_unsetenv, "unsetenv", UnsetenvFn);
#undef GET
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("fork", Fork);
  E("vfork", Fork);
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
  // fd/env/dir calls, child-aware; they delegate to the originals above.
  E("open", Open);
  E("open64", Open);
  E("__open_2", Open);
  E("openat", Openat);
  E("openat64", Openat);
  E("__openat_2", Openat);
  E("close", Close);
  E("dup2", Dup2);
  E("dup3", Dup3);
  E("chdir", Chdir);
  E("setenv", Setenv);
  E("unsetenv", Unsetenv);
  E("putenv", Putenv);
#undef E
}

}  // namespace gesher::bionic
