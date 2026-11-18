// A bounded Win32-only owner for the component's executable and descendants.
// Assignment happens while the root is suspended; no PID-based cleanup and no
// unrelated processes are queried or terminated.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
class Handle {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() { Reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
  HANDLE Get() const { return value_; }
  void Reset() { if (Valid()) CloseHandle(value_); value_ = nullptr; }
 private:
  HANDLE value_;
};

std::wstring Quote(const wchar_t* argument) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (const wchar_t* at = argument; *at; ++at) {
    if (*at == L'\\') { ++slashes; continue; }
    result.append(*at == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    result += *at;
  }
  result.append(slashes * 2, L'\\');
  return result + L'\"';
}

int Error(const char* operation) {
  fprintf(stderr, "COMPONENT_CONTROLLER_ERROR operation=%s win32=%lu\n", operation, GetLastError());
  return 125;
}

bool Active(HANDLE job, DWORD* active) {
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
  if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                 &information, sizeof(information), nullptr)) return false;
  *active = information.ActiveProcesses;
  return true;
}

int Run(int argc, wchar_t** argv) {
  if (argc < 4) return 2;
  wchar_t* end = nullptr;
  const unsigned long timeout = wcstoul(argv[1], &end, 10);
  if (!end || *end || timeout == 0 || timeout > 600000) return 2;
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  Handle log(CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                         &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!log.Valid() || !input.Valid() || !job.Valid()) return Error("create owned handles");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                            JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 16;
  if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) return Error("set owned job limits");
  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  if (!attribute_size) return Error("size inheritance list");
  std::vector<unsigned char> storage(attribute_size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_size)) return Error("create inheritance list");
  struct Attributes {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~Attributes() { DeleteProcThreadAttributeList(list); }
  } cleanup{list};
  HANDLE inherited[] = {input.Get(), log.Get()};
  if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherited, sizeof(inherited), nullptr, nullptr)) return Error("filter inherited handles");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.Get();
  startup.StartupInfo.hStdOutput = log.Get();
  startup.StartupInfo.hStdError = log.Get();
  startup.lpAttributeList = list;
  std::wstring command;
  for (int argument = 3; argument < argc; ++argument) {
    if (!command.empty()) command += L' ';
    command += Quote(argv[argument]);
  }
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(argv[3], command.data(), nullptr, nullptr, TRUE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                      nullptr, nullptr, &startup.StartupInfo, &process)) return Error("create suspended root");
  Handle root(process.hProcess), thread(process.hThread);
  if (!AssignProcessToJobObject(job.Get(), root.Get())) {
    const DWORD error = GetLastError();
    // Assignment failed before the root could execute or create a child.
    const bool terminated = TerminateProcess(root.Get(), 125) != 0;
    const bool reaped = WaitForSingleObject(root.Get(), 5000) == WAIT_OBJECT_0;
    SetLastError(error);
    Error("assign suspended root");
    if (!terminated || !reaped) fprintf(stderr, "COMPONENT_CONTROLLER_ERROR unassigned root cleanup unproved\n");
    return 125;
  }
  int result = 125;
  DWORD root_code = 125;
  const ULONGLONG start = GetTickCount64();
  if (ResumeThread(thread.Get()) == 1) {
    for (;;) {
      const DWORD wait = WaitForSingleObject(root.Get(), 25);
      if (wait == WAIT_OBJECT_0) {
        if (GetExitCodeProcess(root.Get(), &root_code)) result = static_cast<int>(root_code);
        else Error("read root exit");
        break;
      }
      if (wait != WAIT_TIMEOUT) { Error("wait root"); break; }
      LARGE_INTEGER size{};
      if (!GetFileSizeEx(log.Get(), &size)) { Error("query output size"); break; }
      if (size.QuadPart > 4 * 1024 * 1024) {
        fprintf(stderr, "COMPONENT_CONTROLLER_ERROR output limit\n");
        result = 124;
        break;
      }
      if (GetTickCount64() - start >= timeout) {
        fprintf(stderr, "COMPONENT_CONTROLLER_ERROR deadline\n");
        result = 124;
        break;
      }
    }
  } else {
    Error("resume assigned root");
  }
  // A signaled root and an empty job are distinct observations. Release our
  // completed process/thread references before sampling job retirement, then
  // allow a bounded accounting drain. A live descendant still fails the run.
  bool root_exited = false;
  auto reap_root = [&] {
    if (!root_exited && WaitForSingleObject(root.Get(), 0) == WAIT_OBJECT_0) {
      if (!GetExitCodeProcess(root.Get(), &root_code)) { Error("read final root exit"); result = 125; }
      root_exited = true;
      thread.Reset();
      root.Reset();
    }
  };
  reap_root();
  DWORD active = 0;
  bool counted = Active(job.Get(), &active);
  const ULONGLONG retirement_start = GetTickCount64();
  while (root_exited && counted && active && GetTickCount64() - retirement_start < 1000) {
    Sleep(10);
    counted = Active(job.Get(), &active);
  }
  const DWORD before_cleanup = active;
  if (!counted) { Error("query owned processes"); result = 125; }
  if (counted && active && result == 0) {
    fprintf(stderr, "COMPONENT_CONTROLLER_ERROR descendants outlived successful root\n");
    result = 125;
  }
  bool terminated = false;
  if (!counted || active) {
    terminated = TerminateJobObject(job.Get(), 124) != 0;
    if (!terminated) { Error("terminate owned job"); result = 125; }
  }
  const ULONGLONG cleanup_start = GetTickCount64();
  bool empty = false;
  do {
    reap_root();
    if (root_exited && Active(job.Get(), &active) && active == 0) { empty = true; break; }
    Sleep(10);
  } while (GetTickCount64() - cleanup_start < 5000);
  if (!empty) { fprintf(stderr, "COMPONENT_CONTROLLER_ERROR cleanup unproved\n"); result = 125; }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(log.Get(), &size) || size.QuadPart > 4 * 1024 * 1024) result = 125;
  printf("COMPONENT_JOB pid=%lu root_exit=%lu result=%d empty=%d terminated=%d active_before_cleanup=%lu elapsed_ms=%llu bytes=%lld\n",
         process.dwProcessId, root_code, result, empty, terminated, before_cleanup,
         static_cast<unsigned long long>(GetTickCount64() - start),
         static_cast<long long>(size.QuadPart));
  return result;
}
}  // namespace

int wmain(int argc, wchar_t** argv) { return Run(argc, argv); }
