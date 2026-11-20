// Gesher: processes of the Gesher device, the Win32 side (see services_zygote.h).
//
// The system server starts app processes itself (a zygote, lite: Windows has
// no fork, so each is a new dalvikvm), and the first launcher starts binderd,
// servicemanager and the system server, detached: no console window, a
// process group of their own, logs in files, and no handle of the starter's
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), so that a console closing or a
// pipe a test runner reads does not depend on them.
#include <windows.h>

#include <cwchar>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "services_zygote.h"
#include "../posix/src/binder_wire.h"

namespace gesher {
namespace zygote {
namespace {

std::mutex g_lock;
std::map<int, HANDLE> g_children;  // processes this one started, for their exit codes
HANDLE g_job = nullptr;
#ifdef GESHER_RESTART_LEAK_NEGATIVE_CONTROL
// Deliberate defect in this temporary diagnostic branch only.
HANDLE g_seeded_restart_leak = nullptr;
#endif

const wchar_t* W(const std::u16string& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

std::u16string U(const wchar_t* s, size_t n) {
  return std::u16string(reinterpret_cast<const char16_t*>(s), n);
}

// NAME of NAME=value, compared without case (Windows' environment names).
bool SameName(const std::u16string& entry, const std::u16string& name) {
  size_t eq = entry.find(u'=', 1);  // "=C:=C:\..." entries start with '='
  std::u16string n = entry.substr(0, eq);
  if (n.size() != name.size()) return false;
  return CompareStringOrdinal(W(n), static_cast<int>(n.size()), W(name),
                              static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

std::u16string Name(const std::u16string& assignment) {
  return assignment.substr(0, assignment.find(u'=', 1));
}

std::u16string EnvironmentBlock(const std::vector<std::u16string>& set,
                                const std::vector<std::u16string>& unset) {
  std::vector<std::u16string> entries;
  if (wchar_t* env = GetEnvironmentStringsW()) {
    for (const wchar_t* p = env; *p != L'\0'; p += wcslen(p) + 1) {
      entries.push_back(U(p, wcslen(p)));
    }
    FreeEnvironmentStringsW(env);
  }
  auto drop = [&entries](const std::u16string& name) {
    for (auto it = entries.begin(); it != entries.end();) {
      it = SameName(*it, name) ? entries.erase(it) : it + 1;
    }
  };
  for (const auto& name : unset) drop(name);
  for (const auto& assignment : set) {
    drop(Name(assignment));
    entries.push_back(assignment);
  }
  std::u16string block;
  for (const auto& e : entries) {
    block += e;
    block.push_back(u'\0');
  }
  block.push_back(u'\0');
  return block;
}

HANDLE Job() {
  if (g_job != nullptr) return g_job;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) {
    CloseHandle(job);
    return nullptr;
  }
  g_job = job;
  return g_job;
}

struct OwnedHandle {
  HANDLE value = nullptr;
  OwnedHandle() = default;
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  ~OwnedHandle() {
#ifdef GESHER_RESTART_LEAK_NEGATIVE_CONTROL
    if (value == g_seeded_restart_leak) return;
#endif
    if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};

struct HandleList {
  std::vector<char> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
  ~HandleList() { if (value != nullptr) DeleteProcThreadAttributeList(value); }

  DWORD Initialize(HANDLE* handles, size_t count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) return GetLastError();
    storage.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return GetLastError();
    value = list;
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    handles, count * sizeof(HANDLE), nullptr, nullptr)) {
      return GetLastError();
    }
    return ERROR_SUCCESS;
  }
};

}  // namespace

int Spawn(const std::u16string& command_line, const std::vector<std::u16string>& set,
          const std::vector<std::u16string>& unset, const std::u16string& log_path,
          bool breakaway) {
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
  // Appending only: stdout and stderr share the handle, and every write goes
  // to the end, also with another process appending to the same log.
  HANDLE log = CreateFileW(W(log_path), FILE_APPEND_DATA | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return -static_cast<int>(GetLastError());
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, 0, nullptr);
  if (nul == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    CloseHandle(log);
    return -static_cast<int>(error);
  }
  HANDLE handles[] = {log, nul};
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  int result = 0;
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                 sizeof(handles), nullptr, nullptr)) {
    result = -static_cast<int>(GetLastError());
  }
  if (result == 0) {
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = attributes;
    std::u16string env = EnvironmentBlock(set, unset);
    std::u16string cmd = command_line;  // CreateProcessW may write to it
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
                  EXTENDED_STARTUPINFO_PRESENT;
    PROCESS_INFORMATION pi = {};
    BOOL ok = FALSE;
    if (breakaway) {
      ok = CreateProcessW(nullptr, reinterpret_cast<wchar_t*>(cmd.data()), nullptr, nullptr,
                          TRUE, flags | CREATE_BREAKAWAY_FROM_JOB,
                          const_cast<char16_t*>(env.data()), nullptr, &si.StartupInfo, &pi);
      cmd = command_line;
    }
    // A job that allows no breakaway: the process stays in it.
    if (!ok) {
      ok = CreateProcessW(nullptr, reinterpret_cast<wchar_t*>(cmd.data()), nullptr, nullptr,
                          TRUE, flags, const_cast<char16_t*>(env.data()), nullptr,
                          &si.StartupInfo, &pi);
    }
    if (ok) {
      CloseHandle(pi.hThread);
      result = static_cast<int>(pi.dwProcessId);
      std::lock_guard<std::mutex> guard(g_lock);
      g_children[result] = pi.hProcess;
    } else {
      result = -static_cast<int>(GetLastError());
    }
    DeleteProcThreadAttributeList(attributes);
  }
  CloseHandle(nul);
  CloseHandle(log);
  return result;
}

uint32_t RestartWithUid(int uid, uint32_t* exit_code) {
  if (uid < 10000 || exit_code == nullptr) return ERROR_INVALID_PARAMETER;
  std::u16string exe = ExePath(), command = CommandLine();
  if (exe.empty() || command.empty()) return ERROR_INVALID_PARAMETER;
  // This is a loop guard, never evidence of package admission. The new VM parses
  // and admits the APK again and checks its native/Binder UID before binding it.
  const std::wstring decimal = std::to_wstring(uid);
  const std::u16string identity = U(decimal.c_str(), decimal.size());
  std::u16string environment = EnvironmentBlock(
      {u"GESHER_UID=" + identity, u"GESHER_BINDER_UID=" + identity,
       u"GESHER_STANDALONE_RESTARTED=1"}, {});

  OwnedHandle input, output, error, job, process, thread;
  const DWORD kinds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  OwnedHandle* copies[] = {&input, &output, &error};
  HANDLE handles[3]{};
  for (size_t i = 0; i < 3; ++i) {
    HANDLE original = GetStdHandle(kinds[i]);
    if (original == nullptr || original == INVALID_HANDLE_VALUE) {
      SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
      copies[i]->value = CreateFileW(L"NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                     OPEN_EXISTING, 0, nullptr);
      if (copies[i]->value == INVALID_HANDLE_VALUE) return GetLastError();
    } else if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                                &copies[i]->value, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
      return GetLastError();
    }
    handles[i] = copies[i]->value;
  }
#ifdef GESHER_RESTART_LEAK_NEGATIVE_CONTROL
  g_seeded_restart_leak = input.value;
#endif
  HandleList attributes;
  DWORD failure = attributes.Initialize(handles, 3);
  if (failure != ERROR_SUCCESS) return failure;
  job.value = CreateJobObjectW(nullptr, nullptr);
  if (job.value == nullptr) return GetLastError();
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits))) return GetLastError();

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.value;
  startup.StartupInfo.hStdError = error.value;
  startup.lpAttributeList = attributes.value;
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(W(exe), reinterpret_cast<wchar_t*>(command.data()), nullptr, nullptr,
                       TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                             EXTENDED_STARTUPINFO_PRESENT,
                       environment.data(), nullptr, &startup.StartupInfo, &child)) {
    return GetLastError();
  }
  process.value = child.hProcess;
  thread.value = child.hThread;
  // Bind lifetime ownership before the child can execute any runtime or APK code.
  if (!AssignProcessToJobObject(job.value, process.value)) {
    failure = GetLastError();
    if (TerminateProcess(process.value, 1)) WaitForSingleObject(process.value, INFINITE);
    return failure;
  }
  // Transfer only the foreground permission Windows actually gives this launcher.
  // Failure leaves the normal OS focus rules in force; it never fabricates focus.
  AllowSetForegroundWindow(child.dwProcessId);
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return GetLastError();
  if (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) return GetLastError();
  DWORD code = 0;
  if (!GetExitCodeProcess(process.value, &code)) return GetLastError();
  *exit_code = code;
  return ERROR_SUCCESS;
}

int64_t StartTime(int pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                         static_cast<DWORD>(pid));
  if (h == nullptr) return 0;
  int64_t result = 0;
  FILETIME created, exited, kernel, user;
  if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT &&
      GetProcessTimes(h, &created, &exited, &kernel, &user)) {
    result = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  }
  CloseHandle(h);
  return result;
}

int ExitCode(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return -1;
  DWORD code = 0;
  if (!GetExitCodeProcess(it->second, &code)) return -1;
  if (code != STILL_ACTIVE) {
    CloseHandle(it->second);
    g_children.erase(it);
  }
  return static_cast<int>(code);
}

int64_t ChildStartTime(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return 0;
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(it->second, &created, &exited, &kernel, &user)) return 0;
  return (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool KillChild(int pid, int64_t start_time) {
  if (pid <= 0 || start_time <= 0) return false;
  // ExitCode cannot close this handle and Spawn cannot replace this map entry
  // between checking its identity and termination. The handle, not the PID,
  // names the original Windows process throughout the operation.
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_children.find(pid);
  if (it == g_children.end()) return false;
  HANDLE child = it->second;
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(child, &created, &exited, &kernel, &user)) return false;
  int64_t actual = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  if (actual != start_time || WaitForSingleObject(child, 0) != WAIT_TIMEOUT) return false;
  return TerminateProcess(child, 137) != FALSE;
}

bool Kill(int pid) {
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  BOOL ok = TerminateProcess(h, 137);
  CloseHandle(h);
  return ok != FALSE;
}

bool JoinJob(int pid) {
  std::lock_guard<std::mutex> guard(g_lock);
  HANDLE job = Job();
  if (job == nullptr) return false;
  HANDLE h = pid == 0 ? GetCurrentProcess()
                      : OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                    static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  BOOL ok = AssignProcessToJobObject(job, h);
  if (pid != 0) CloseHandle(h);
  return ok != FALSE;
}

intptr_t Lock(const std::u16string& name, int timeout_ms) {
  HANDLE m = CreateMutexW(nullptr, FALSE, W(name));
  if (m == nullptr) return 0;
  DWORD r = WaitForSingleObject(m, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  // WAIT_ABANDONED: the last holder died holding it; it is ours now.
  if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) return reinterpret_cast<intptr_t>(m);
  CloseHandle(m);
  return 0;
}

void Unlock(intptr_t handle) {
  if (handle == 0) return;
  ReleaseMutex(reinterpret_cast<HANDLE>(handle));
  CloseHandle(reinterpret_cast<HANDLE>(handle));
}

intptr_t MakeEvent(const std::u16string& name) {
  return reinterpret_cast<intptr_t>(CreateEventW(nullptr, FALSE, FALSE, W(name)));
}

bool WaitEvent(intptr_t handle, int timeout_ms) {
  if (handle == 0) return false;
  return WaitForSingleObject(reinterpret_cast<HANDLE>(handle),
                             timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms)) ==
         WAIT_OBJECT_0;
}

bool SignalEvent(const std::u16string& name) {
  HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, W(name));
  if (e == nullptr) return false;
  BOOL ok = SetEvent(e);
  CloseHandle(e);
  return ok != FALSE;
}

bool WaitForBinder(const std::u16string& device, int broker_pid, int timeout_ms) {
  if (device.empty() || broker_pid <= 0 || timeout_ms < 0) return false;
  struct ScopedHandle {
    HANDLE value;
    ~ScopedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  };
  ScopedHandle broker{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, broker_pid)};
  if (!broker.value) return false;
  uint64_t deadline = GetTickCount64() + static_cast<uint64_t>(timeout_ms);
  auto remaining = [&]() -> DWORD {
    uint64_t now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
  };
  std::wstring name = binder_wire::kPipePrefix;
  name.append(reinterpret_cast<const wchar_t*>(device.data()), device.size());
  ScopedHandle pipe{INVALID_HANDLE_VALUE};
  for (;;) {
    if (WaitForSingleObject(broker.value, 0) != WAIT_TIMEOUT) return false;
    pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe.value != INVALID_HANDLE_VALUE) break;
    DWORD error = GetLastError(), left = remaining();
    if (!left || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)) return false;
    if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), std::min<DWORD>(left, 20));
    else if (WaitForSingleObject(broker.value, std::min<DWORD>(left, 10)) != WAIT_TIMEOUT) return false;
  }
  ULONG server_pid = 0;
  if (!GetNamedPipeServerProcessId(pipe.value, &server_pid) || server_pid != static_cast<ULONG>(broker_pid)) return false;
  ScopedHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.value) return false;
  auto transfer = [&](void* bytes, DWORD size, bool write) {
    auto* cursor = static_cast<unsigned char*>(bytes);
    while (size) {
      OVERLAPPED operation{};
      operation.hEvent = event.value;
      ResetEvent(event.value);
      DWORD count = 0;
      BOOL done = write ? WriteFile(pipe.value, cursor, size, &count, &operation)
                        : ReadFile(pipe.value, cursor, size, &count, &operation);
      if (!done) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[]{broker.value, event.value};
        DWORD result = WaitForMultipleObjects(2, waits, FALSE, remaining());
        if (result != WAIT_OBJECT_0 + 1) {
          CancelIoEx(pipe.value, &operation);
          GetOverlappedResult(pipe.value, &operation, &count, TRUE);
          return false;
        }
        if (!GetOverlappedResult(pipe.value, &operation, &count, FALSE)) return false;
      }
      if (!count) return false;
      size -= count;
      cursor += count;
    }
    return true;
  };
  uint32_t hello[]{5 * sizeof(uint32_t), binder_wire::kHelloProc, binder_wire::kMagic,
                    binder_wire::kVersion, GetCurrentProcessId(), 10000};
  uint32_t length = 0;
  unsigned char reply[16]{};
  if (!transfer(hello, sizeof(hello), true) || !transfer(&length, sizeof(length), false) ||
      length != sizeof(reply) || !transfer(reply, sizeof(reply), false)) return false;
  int32_t status;
  uint32_t process;
  std::memcpy(&status, reply, sizeof(status));
  std::memcpy(&process, reply + 4, sizeof(process));
  return status == 0 && process != 0 && WaitForSingleObject(broker.value, 0) == WAIT_TIMEOUT;
}

std::u16string CommandLine() {
  const wchar_t* cmd = GetCommandLineW();
  return U(cmd, wcslen(cmd));
}

std::u16string ExePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return std::u16string();
    if (n < buffer.size()) return U(buffer.data(), n);
    buffer.resize(buffer.size() * 2);
  }
}

}  // namespace zygote
}  // namespace gesher
