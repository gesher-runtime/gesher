// Actual Windows process/stdio/job boundary for standalone UID re-entry.
// The services components/net fixtures separately run Main, PackageManager and ART with
// a later installed package, whose canonical UID differs from the default 10000.
#include <windows.h>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include "../../compat/framework/services_zygote.h"
#ifdef GESHER_RESTART_AUDIT
#include "restart_resource_audit.h"
#endif

namespace {
int checks = 0;
void Check(bool value, const char* what) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL %s (Win32 %lu)\n", what, GetLastError());
    std::fflush(stderr);
    ExitProcess(1);
  }
}
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct File {
  std::wstring path;
  File() {
    wchar_t directory[MAX_PATH], name[MAX_PATH];
    Check(GetTempPathW(MAX_PATH, directory) != 0, "temporary directory");
    Check(GetTempFileNameW(directory, L"gsi", 0, name) != 0, "temporary file");
    path = name;
  }
  ~File() { DeleteFileW(path.c_str()); }
  HANDLE Open(DWORD access) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE result = CreateFileW(path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, OPEN_EXISTING, 0, nullptr);
    Check(result != INVALID_HANDLE_VALUE, "open fixture standard handle");
    return result;
  }
  std::string Read() {
    Handle handle;
    handle.value = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(handle.value != INVALID_HANDLE_VALUE, "read fixture output");
    char buffer[16384];
    DWORD count = 0;
    Check(ReadFile(handle.value, buffer, sizeof(buffer), &count, nullptr) != FALSE, "read output bytes");
    return std::string(buffer, count);
  }
};

bool ReadChildIdentity(const std::string& report, DWORD* pid, unsigned long long* birth) {
  constexpr char prefix[] = "child pid=";
  constexpr char separator[] = " generation=";
  auto at = report.find(prefix);
  if (at == std::string::npos) return false;
  const char* end = report.data() + report.size();
  DWORD parsed_pid = 0;
  auto pid_result = std::from_chars(report.data() + at + sizeof(prefix) - 1, end, parsed_pid);
  if (pid_result.ec != std::errc{} || parsed_pid == 0
      || static_cast<size_t>(end - pid_result.ptr) < sizeof(separator) - 1
      || std::memcmp(pid_result.ptr, separator, sizeof(separator) - 1) != 0) return false;
  unsigned long long parsed_birth = 0;
  auto birth_result = std::from_chars(pid_result.ptr + sizeof(separator) - 1, end, parsed_birth);
  // A trailing space proves the numeric field is complete, even while polling a partial write.
  if (birth_result.ec != std::errc{} || parsed_birth == 0
      || birth_result.ptr == end || *birth_result.ptr != ' ') return false;
  *pid = parsed_pid;
  *birth = parsed_birth;
  return true;
}

void Worker(uint32_t expected, const wchar_t* payload) {
  Check(std::wcscmp(payload, L"argument with spaces \u05d0") == 0, "same Unicode arguments");
  wchar_t restarted[2]{};
  if (GetEnvironmentVariableW(L"GESHER_STANDALONE_RESTARTED", restarted, 2) == 1
      && restarted[0] == L'1') {
    wchar_t uid[6]{}, binder[6]{};
    DWORD uid_length = GetEnvironmentVariableW(L"GESHER_UID", uid, 6);
    DWORD binder_length = GetEnvironmentVariableW(L"GESHER_BINDER_UID", binder, 6);
    Check(uid_length == 5 && std::wcscmp(uid, L"10037") == 0, "UID before child code");
    Check(binder_length == 5 && std::wcscmp(binder, uid) == 0, "matching Binder environment");
    char input[64]{};
    Check(std::fgets(input, sizeof(input), stdin) != nullptr
        && std::strcmp(input, "original input\n") == 0, "original stdin bytes");
    unsigned long pid = GetCurrentProcessId();
    auto birth = static_cast<unsigned long long>(gesher::zygote::StartTime(static_cast<int>(pid)));
    Check(birth != 0, "actual child generation");
    std::printf("child pid=%lu generation=%llu input=original input\n", pid, birth);
    std::fprintf(stderr, "child stderr remains separate\n");
    std::fflush(stdout); std::fflush(stderr);
    if (expected == 99) Sleep(INFINITE);  // Parent-death test owns this process and its job.
    ExitProcess(expected);
  }
  DWORD before = 0, after = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "initial handle count");
  std::printf("supervisor before restart\n"); std::fflush(stdout);
#ifdef GESHER_RESTART_AUDIT
  gesher::zygote::RestartAudit("after-first-stdio", nullptr, TRUE, ERROR_SUCCESS);
  restart_audit::Entries initial_handles;
  DWORD capture = restart_audit::Capture(&initial_handles);
  gesher::zygote::RestartAudit("initial-snapshot-cleaned", nullptr, capture == ERROR_SUCCESS, capture);
  Check(capture == ERROR_SUCCESS, "capture initial diagnostic handle types");
#endif
  uint32_t result = 0;
  uint32_t restart_error = gesher::zygote::RestartWithUid(10037, &result);
  if (restart_error != 0) {
    std::fprintf(stderr, "restart returned Win32 error %u (expected child exit %u)\n",
        restart_error, expected);
    std::fflush(stderr);
  }
  Check(restart_error == 0, "native restart");
  BOOL counted = GetProcessHandleCount(GetCurrentProcess(), &after);
  if (!counted || before != after) {
    DWORD error = counted ? ERROR_SUCCESS : GetLastError();
    std::fprintf(stderr, "restart handles before=%lu after=%lu counted=%d error=%lu child-exit=%u\n",
        before, after, counted, error, result);
    std::fflush(stderr);
#ifdef GESHER_RESTART_AUDIT
    restart_audit::Entries first_handles;
    capture = restart_audit::Capture(&first_handles);
    gesher::zygote::RestartAudit("first-call-snapshot-cleaned", nullptr,
        capture == ERROR_SUCCESS, capture);
    Check(capture == ERROR_SUCCESS, "capture first-call diagnostic handle types");
    restart_audit::Delta("first-call", initial_handles, first_handles);
    // Keep the original first-call comparison below fatal. This additional real
    // call only distinguishes one-time initialization from continuing growth.
    if (expected == 0) {
      LARGE_INTEGER rewind{};
      Check(SetFilePointerEx(GetStdHandle(STD_INPUT_HANDLE), rewind, nullptr, FILE_BEGIN) != FALSE,
          "rewind real stdin for resource diagnostic repetition");
      uint32_t repeated_exit = 0;
      uint32_t repeated_error = gesher::zygote::RestartWithUid(10037, &repeated_exit);
      std::printf("restart audit repetition native-error=%u child-exit=%u\n",
          repeated_error, repeated_exit);
      restart_audit::Entries repeated_handles;
      capture = restart_audit::Capture(&repeated_handles);
      Check(capture == ERROR_SUCCESS, "capture repeated diagnostic handle types");
      restart_audit::Delta("repetition", first_handles, repeated_handles);
    }
#endif
  }
  Check(counted != FALSE && before == after,
      "restart releases process, thread, job, stdio and attribute resources");
  std::printf("supervisor observed exact exit %u and released handles\n", result);
  std::fflush(stdout);
  ExitProcess(result);
}

void Run(uint32_t expected) {
  std::printf("Restart case expected=%u (0x%08x)\n", expected, expected);
  std::fflush(stdout);
  File input, output, error;
  Handle in, out, err, job, process, thread;
  in.value = input.Open(GENERIC_READ | GENERIC_WRITE);
  out.value = output.Open(GENERIC_WRITE);
  err.value = error.Open(GENERIC_WRITE);
  constexpr char text[] = "original input\n";
  DWORD written = 0;
  Check(WriteFile(in.value, text, sizeof(text) - 1, &written, nullptr) != FALSE,
      "write fixture stdin");
  LARGE_INTEGER zero{};
  Check(SetFilePointerEx(in.value, zero, nullptr, FILE_BEGIN) != FALSE, "rewind fixture stdin");
  job.value = CreateJobObjectW(nullptr, nullptr);
  Check(job.value != nullptr, "fixture lifetime job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  Check(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
      sizeof(limits)) != FALSE, "fixture owns all descendants on failure");
  auto executable = gesher::zygote::ExePath();
  std::wstring command = L"\"" + std::wstring(executable.begin(), executable.end())
      + L"\" --worker " + std::to_wstring(expected) + L" \"argument with spaces \u05d0\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = in.value; startup.hStdOutput = out.value; startup.hStdError = err.value;
  PROCESS_INFORMATION child{};
  Check(CreateProcessW(reinterpret_cast<const wchar_t*>(executable.c_str()), command.data(),
      nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child) != FALSE,
      "start real supervisor process");
  process.value = child.hProcess; thread.value = child.hThread;
  Check(AssignProcessToJobObject(job.value, process.value) != FALSE, "fixture job enrollment");
  Check(ResumeThread(thread.value) != static_cast<DWORD>(-1), "run supervisor");
  if (expected == 99) {
    DWORD pid = 0;
    unsigned long long birth = 0;
    for (int i = 0; i < 200; ++i) {
      std::string report = output.Read();
      if (ReadChildIdentity(report, &pid, &birth)) break;
      Sleep(25);
    }
    Check(pid != 0 && birth != 0, "parked child reported actual identity");
    Handle descendant;
    descendant.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    Check(descendant.value != nullptr, "retain parked child handle");
    FILETIME created, exited, kernel, user;
    Check(GetProcessTimes(descendant.value, &created, &exited, &kernel, &user) != FALSE,
        "read retained generation");
    Check(((static_cast<unsigned long long>(created.dwHighDateTime) << 32)
        | created.dwLowDateTime) == birth, "retained child is the reported generation");
    Check(TerminateProcess(process.value, 77) != FALSE, "end supervisor through owned handle");
    Check(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "supervisor ended");
    Check(WaitForSingleObject(descendant.value, 5000) == WAIT_OBJECT_0,
        "supervisor death retires the relaunched child");
  } else {
    Check(WaitForSingleObject(process.value, 15000) == WAIT_OBJECT_0, "restart completed");
    DWORD code = 0;
    BOOL queried = GetExitCodeProcess(process.value, &code);
    DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
    if (!queried || code != expected
#ifdef GESHER_RESTART_AUDIT
        || expected == 0
#endif
        ) {
      std::string child_output = output.Read(), child_error = error.Read();
      std::fprintf(stderr,
          "restart exit expected=%u (0x%08x) observed=%lu (0x%08lx) queried=%d error=%lu supervisor=%lu\n",
          expected, expected, code, code, queried, query_error, child.dwProcessId);
      std::fprintf(stderr, "captured stdout (%zu bytes):\n", child_output.size());
      std::fwrite(child_output.data(), 1, child_output.size(), stderr);
      std::fprintf(stderr, "\ncaptured stderr (%zu bytes):\n", child_error.size());
      std::fwrite(child_error.data(), 1, child_error.size(), stderr);
      std::fprintf(stderr, "\n");
      std::fflush(stderr);
    }
    Check(queried != FALSE && code == expected,
        "exact native exit status (including high bit and 259)");
    std::string stdout_text = output.Read(), stderr_text = error.Read();
    Check(stdout_text.find("input=original input") != std::string::npos,
        "restarted child inherited input and output");
    Check(stdout_text.find("released handles") != std::string::npos,
        "supervisor observed complete resource release");
    Check(stderr_text.find("child stderr remains separate") != std::string::npos
        && stdout_text.find("child stderr") == std::string::npos, "stderr destination preserved");
  }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wcscmp(argv[1], L"--worker") == 0) {
    Worker(static_cast<uint32_t>(std::wcstoull(argv[2], nullptr, 10)), argv[3]);
  }
  Check(argc == 1, "fixture arguments");
  Check(SetEnvironmentVariableW(L"GESHER_STANDALONE_RESTARTED", nullptr) != FALSE,
      "clear fixture restart marker");
  uint32_t unchanged = 123;
  Check(gesher::zygote::RestartWithUid(999, &unchanged) == ERROR_INVALID_PARAMETER
      && unchanged == 123, "invalid application UID does not launch or fabricate status");
  for (uint32_t code : {0u, 17u, 259u, 0x80000000u, 99u}) Run(code);
  std::printf("Standalone native restart all passed: %d checks\n", checks);
  return 0;
}
