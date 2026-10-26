// Gesher: java.lang.UNIXProcess's natives on Windows (docs/exec.md,
// docs/decisions/0035-exec-elf-processes.md).
//
// libcore's ProcessBuilder.start() and Runtime.exec() end in UNIXProcess,
// whose natives (ojluni's UNIXProcess_md.c) fork and exec. Windows has
// neither, so this file implements the same four natives with CreateProcessW:
//
//   forkAndExec   finds the program as execvp would and starts it
//                 (compat/elf/exec/exec_spawn.h: an ELF executable runs in
//                 gesher-exec.exe, which loads it with the ELF loader; .exe,
//                 .cmd/.bat and sh have their own ways), with stdin, stdout and
//                 stderr as pipes whose parent ends are gesher-base fds (CRT
//                 fds over the pipe handles), or the fds/files ProcessBuilder
//                 redirects to; the environment block and working directory;
//                 returns the Windows process id as the pid;
//   waitForProcessExit  waits for it and returns its exit status as Linux's
//                 waitpid would give it to Java (exit code, or 0x80 + signal);
//   destroyProcess  terminates it with status 0x80 + SIGTERM;
//   initIDs       nothing to do.
//
// An IOException carries "error=<errno>, <text>" as on Linux, so
// ProcessBuilder's message is the familiar "Cannot run program ...:
// error=2, No such file or directory".

#include <fcntl.h>
#include <io.h>
#include <stdio.h>

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "exec_spawn.h"

// gesher-base: kill() of another process through its signal mailbox
// (compat/posix/src/process.cc).
extern "C" int gesher_kill_other(int pid, int sig);

namespace ex = gesher::exec;

namespace {

constexpr int kSigterm = 15;
constexpr int kSigkill = 9;
// Pipe buffer for the child's stdin/stdout/stderr (Linux's is 64 KiB).
constexpr DWORD kPipeBuffer = 256 * 1024;

std::mutex g_mutex;
std::map<jint, HANDLE> g_children;  // pid -> process handle, until waited for

// The program being started, for the log line of a failure.
thread_local const char* t_program = "";

void ThrowIOException(JNIEnv* env, int error, const char* what = nullptr) {
  char message[512];
  snprintf(message, sizeof(message), "error=%d, %s", error, what != nullptr ? what : ex::ErrorText(error));
  fprintf(stderr, "gesher: exec %s failed: %s\n", t_program, message);
  fflush(stderr);
  jclass c = env->FindClass("java/io/IOException");
  if (c != nullptr) env->ThrowNew(c, message);
}

std::string ByteString(JNIEnv* env, jbyteArray a) {
  if (a == nullptr) return std::string();
  jsize n = env->GetArrayLength(a);
  std::string s(static_cast<size_t>(n), '\0');
  env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(s.data()));
  return s;
}

// `count` NUL-terminated strings packed in `block`.
std::vector<std::string> Strings(const std::string& block, int count) {
  std::vector<std::string> out;
  size_t pos = 0;
  for (int i = 0; i < count && pos <= block.size(); ++i) {
    size_t end = block.find('\0', pos);
    if (end == std::string::npos) end = block.size();
    out.push_back(block.substr(pos, end - pos));
    pos = end + 1;
  }
  return out;
}

HANDLE Inheritable(HANDLE h) {
  HANDLE dup = nullptr;
  if (h == nullptr || h == INVALID_HANDLE_VALUE ||
      !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
    // No usable handle behind the fd (a GUI process's missing console): NUL.
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    dup = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                      0, nullptr);
    if (dup == INVALID_HANDLE_VALUE) dup = nullptr;
  }
  return dup;
}

void CloseIf(HANDLE* h) {
  if (*h != nullptr && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
  *h = nullptr;
}

jint JNICALL ForkAndExec(JNIEnv* env, jobject, jbyteArray prog, jbyteArray arg_block, jint argc,
                         jbyteArray env_block, jint envc, jbyteArray dir_bytes, jintArray std_fds,
                         jboolean redirect_error_stream) {
  std::string program = ByteString(env, prog);
  if (!program.empty() && program.back() == '\0') program.pop_back();
  t_program = program.c_str();
  std::vector<std::string> argv{program};
  for (std::string& a : Strings(ByteString(env, arg_block), argc)) argv.push_back(std::move(a));
  std::string dir = ByteString(env, dir_bytes);
  if (!dir.empty() && dir.back() == '\0') dir.pop_back();

  // The working directory must exist (the child's chdir on Linux).
  std::wstring cwd;
  if (dir_bytes != nullptr) {
    cwd = ex::WindowsPath(dir);
    DWORD attributes = GetFileAttributesW(cwd.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return ThrowIOException(env, ex::kENOENT), -1;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return ThrowIOException(env, ex::kENOTDIR), -1;
  }

  ex::Launch launch;
  int error = ex::PrepareLaunch(argv, dir, &launch);
  if (error == 0 && launch.kind != "exe" && launch.kind != "batch" &&
      GetFileAttributesW(launch.application.c_str()) == INVALID_FILE_ATTRIBUTES) {
    fprintf(stderr, "gesher: exec %s: the launcher %s is missing\n", program.c_str(),
            ex::Narrow(launch.application).c_str());
    error = ex::kENOEXEC;
  }
  if (error != 0) return ThrowIOException(env, error), -1;

  std::wstring environment;
  if (env_block != nullptr) environment = ex::EnvironmentBlock(Strings(ByteString(env, env_block), envc));

  jint fds[3];
  env->GetIntArrayRegion(std_fds, 0, 3, fds);
  HANDLE child[3] = {nullptr, nullptr, nullptr};
  HANDLE parent[3] = {nullptr, nullptr, nullptr};
  auto cleanup = [&] {
    for (int i = 0; i < 3; ++i) {
      CloseIf(&child[i]);
      CloseIf(&parent[i]);
    }
  };
  for (int i = 0; i < 3; ++i) {
    if (i == 2 && redirect_error_stream) continue;  // stderr goes where stdout goes
    if (fds[i] == -1) {
      HANDLE read_end = nullptr, write_end = nullptr;
      if (!CreatePipe(&read_end, &write_end, nullptr, kPipeBuffer)) {
        int e = ex::ErrnoFromWin32(GetLastError());
        cleanup();
        return ThrowIOException(env, e, "Bad file descriptor"), -1;
      }
      child[i] = i == 0 ? read_end : write_end;
      parent[i] = i == 0 ? write_end : read_end;
      SetHandleInformation(child[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    } else {
      child[i] = Inheritable(reinterpret_cast<HANDLE>(_get_osfhandle(fds[i])));
    }
  }

  PROCESS_INFORMATION pi = {};
  DWORD spawn_error = ex::Spawn(launch, environment.empty() ? nullptr : environment.c_str(), cwd, child[0],
                                child[1], redirect_error_stream ? child[1] : child[2], &pi);
  for (HANDLE& h : child) CloseIf(&h);
  if (spawn_error != 0) {
    cleanup();
    return ThrowIOException(env, ex::ErrnoFromWin32(spawn_error)), -1;
  }
  CloseHandle(pi.hThread);
  jint pid = static_cast<jint>(pi.dwProcessId);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_children[pid] = pi.hProcess;
  }

  // The parent's ends become fds that FileInputStream/FileOutputStream own.
  for (int i = 0; i < 3; ++i) {
    if (parent[i] == nullptr) {
      fds[i] = -1;
      continue;
    }
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(parent[i]),
                             (i == 0 ? _O_WRONLY : _O_RDONLY) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) CloseIf(&parent[i]);
    fds[i] = fd;
    parent[i] = nullptr;
  }
  env->SetIntArrayRegion(std_fds, 0, 3, fds);
  fprintf(stderr, "gesher: exec %s (%s) started as pid %d\n", program.c_str(), launch.kind.c_str(),
          static_cast<int>(pid));
  fflush(stderr);
  return pid;
}

jint JNICALL WaitForProcessExit(JNIEnv*, jobject, jint pid) {
  HANDLE h = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_children.find(pid);
    if (it == g_children.end()) return 0;  // waitpid's ECHILD: the JDK answers 0
    h = it->second;
  }
  WaitForSingleObject(h, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(h, &code);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_children.erase(pid);
    CloseHandle(h);
  }
  int status = ex::ExitStatus(code);
  fprintf(stderr, "gesher: exec pid %d exited with status %d\n", static_cast<int>(pid), status);
  fflush(stderr);
  return status;
}

// Process.destroy(): SIGTERM through gesher-base's signal mailbox
// (compat/posix/src/process.cc), so a Gesher child runs its own SIGTERM
// handler (the launched program's, delivered by the bionic shim); a child
// without a mailbox (a plain Windows program) is terminated with 143 by
// gesher-base's default action, as before.
void JNICALL DestroyProcess(JNIEnv*, jclass, jint pid) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_children.find(pid) == g_children.end()) return;
  }
  gesher_kill_other(pid, kSigterm);
}

// Process.destroyForcibly(): SIGKILL (TerminateProcess), the reaper sees
// 0x80 + 9 = 137.
void JNICALL DestroyProcessForcibly(JNIEnv*, jclass, jint pid) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_children.find(pid);
  if (it != g_children.end()) TerminateProcess(it->second, 0x80 + kSigkill);
}

void JNICALL InitIDs(JNIEnv*, jclass) {}

const JNINativeMethod kMethods[] = {
    {"destroyProcess", "(I)V", reinterpret_cast<void*>(&DestroyProcess)},
    {"destroyProcessForcibly", "(I)V", reinterpret_cast<void*>(&DestroyProcessForcibly)},
    {"forkAndExec", "([B[BI[BI[B[IZ)I", reinterpret_cast<void*>(&ForkAndExec)},
    {"waitForProcessExit", "(I)I", reinterpret_cast<void*>(&WaitForProcessExit)},
    {"initIDs", "()V", reinterpret_cast<void*>(&InitIDs)},
};

}  // namespace

// Called by ojluni's OnLoad.cpp, in place of UNIXProcess_md.c's.
extern "C" void register_java_lang_UNIXProcess(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/UNIXProcess", kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}
