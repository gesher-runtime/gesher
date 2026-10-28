// Disposable Win32 boundary experiment. No Gesher guest launch path uses this.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <string>
#include <vector>

struct Handle {
  HANDLE h = nullptr;
  ~Handle() { reset(); }
  void reset(HANDLE next = nullptr) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = next;
  }
  bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};
static bool denied(HANDLE h) {
  const DWORD error = GetLastError();
  if (h != nullptr && h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
  return error == ERROR_ACCESS_DENIED;
}
static bool token_is(HANDLE process, PSID expected) {
  Handle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token.h)) return false;
  DWORD size = 0, isolated = 0;
  if (!GetTokenInformation(token.h, TokenIsAppContainer, &isolated, sizeof(isolated), &size) || isolated != 1) return false;
  GetTokenInformation(token.h, TokenAppContainerSid, nullptr, 0, &size);
  std::vector<BYTE> buffer(size);
  if (!size || !GetTokenInformation(token.h, TokenAppContainerSid, buffer.data(), size, &size)) return false;
  return EqualSid(expected, reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer.data())->TokenAppContainer) != FALSE;
}
static bool write_bytes(HANDLE h, const char* text, DWORD size) {
  DWORD n = 0;
  return WriteFile(h, text, size, &n, nullptr) && n == size;
}
static constexpr char kRows[] =
    "PASS child actual-token-sid\n"
    "PASS child host-process-dup-denied error=5\n"
    "PASS child host-process-write-denied error=5\n"
    "PASS child host-process-thread-denied error=5\n"
    "PASS child host-thread-context-denied error=5\n"
    "PASS child host-thread-suspend-denied error=5\n"
    "PASS child trusted-code-write-denied error=5\n"
    "PASS child trusted-config-write-denied error=5\n"
    "PASS child host-ipc-denied error=5\n"
    "PASS child explicit-resource-pipe challenge=Gesher-resource-v1\n";

static int child(int argc, wchar_t** argv) {
  if (argc != 10) return 10;
  PSID expected = nullptr;
  if (!ConvertStringSidToSidW(argv[2], &expected)) return 11;
  const bool actual = token_is(GetCurrentProcess(), expected);
  LocalFree(expected);
  if (!actual) return 12;
  const DWORD pid = wcstoul(argv[3], nullptr, 10), tid = wcstoul(argv[4], nullptr, 10);
  for (DWORD rights : {PROCESS_DUP_HANDLE, PROCESS_VM_WRITE, PROCESS_CREATE_THREAD})
    if (!denied(OpenProcess(rights, FALSE, pid))) return 13;
  for (DWORD rights : {THREAD_SET_CONTEXT, THREAD_SUSPEND_RESUME})
    if (!denied(OpenThread(rights, FALSE, tid))) return 14;
  for (int i = 5; i <= 6; ++i) {
    // A readable existing resource, so a missing path cannot satisfy denial.
    Handle readable;
    readable.h = CreateFileW(argv[i], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!readable.valid()) return 15;
    readable.reset();
    if (!denied(CreateFileW(argv[i], GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))) return 16;
  }
  if (!denied(CreateFileW(argv[7], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr))) return 17;
  Handle input, output;
  input.h = reinterpret_cast<HANDLE>(_wcstoui64(argv[8], nullptr, 10));
  output.h = reinterpret_cast<HANDLE>(_wcstoui64(argv[9], nullptr, 10));
  char challenge[20]{}; DWORD n = 0;
  if (!ReadFile(input.h, challenge, sizeof(challenge)-1, &n, nullptr) || n != 19 ||
      memcmp(challenge, "Gesher-resource-v1\n", 19) != 0) return 18;
  return write_bytes(output.h, kRows, sizeof(kRows)-1) ? 0 : 19;
}

struct Fixture {
  std::wstring root, executable, code, config, profile;
  PSID sid = nullptr;
  bool directory = false, profile_created = false;
  bool child_stopped = true;
  bool cleanup() {
    bool ok = child_stopped;
    for (const auto* path : {&executable, &code, &config}) {
      if (!path->empty() && !DeleteFileW(path->c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = false;
    }
    if (directory && !RemoveDirectoryW(root.c_str())) ok = false;
    directory = false;
    if (profile_created && FAILED(DeleteAppContainerProfile(profile.c_str()))) ok = false;
    profile_created = false;
    if (sid) FreeSid(sid);
    sid = nullptr;
    return ok;
  }
  ~Fixture() { cleanup(); }
};
static bool set_acl(const std::wstring& path, const std::wstring& sddl, SE_OBJECT_TYPE type) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
  PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  bool ok = GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) && present;
  if (ok) ok = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), type,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
  LocalFree(sd); return ok;
}
static bool marker(const std::wstring& path) {
  Handle f;
  f.h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  return f.valid() && write_bytes(f.h, "trusted-v1", 10);
}
static bool unchanged(const std::wstring& path) {
  Handle f;
  f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  char buf[11]{}; DWORD n = 0;
  return f.valid() && ReadFile(f.h, buf, sizeof(buf), &n, nullptr) && n == 10 && memcmp(buf, "trusted-v1", 10) == 0;
}
static int exercise(Fixture& f) {
  for (DWORD rights : {PROCESS_DUP_HANDLE, PROCESS_VM_WRITE, PROCESS_CREATE_THREAD}) {
    Handle positive; positive.h = OpenProcess(rights, FALSE, GetCurrentProcessId());
    if (!positive.valid()) return 51;
  }
  for (DWORD rights : {THREAD_SET_CONTEXT, THREAD_SUSPEND_RESUME}) {
    Handle positive; positive.h = OpenThread(rights, FALSE, GetCurrentThreadId());
    if (!positive.valid()) return 52;
  }
  wchar_t exe[32768], temp[32768], unique[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, 32768) || !GetTempPathW(32768, temp) ||
      !GetTempFileNameW(temp, L"gsi", 0, unique)) return 20;
  // Replace only our freshly created unique temporary file with our directory.
  if (!DeleteFileW(unique) || !CreateDirectoryW(unique, nullptr)) return 21;
  f.root = unique; f.directory = true;
  f.executable = f.root + L"\\probe.exe";
  f.code = f.root + L"\\trusted-code.bin";
  f.config = f.root + L"\\trusted-config.ini";
  f.profile = L"GesherIsolation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  if (FAILED(CreateAppContainerProfile(f.profile.c_str(), f.profile.c_str(), f.profile.c_str(), nullptr, 0, &f.sid))) return 22;
  f.profile_created = true;
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(f.sid, &text)) return 23;
  std::wstring sid_text = text; LocalFree(text);
  // Trusted same-user host is outside this experiment's adversary boundary.
  // OW grants our owner full access; ONLY this AppContainer receives read/execute.
  const std::wstring acl = L"D:P(A;OICI;FA;;;OW)(A;OICI;GRGX;;;" + sid_text + L")";
  if (!set_acl(f.root, acl, SE_FILE_OBJECT) || !CopyFileW(exe, f.executable.c_str(), TRUE) ||
      !marker(f.code) || !marker(f.config)) return 24;
  const std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\" + f.profile;
  PSECURITY_DESCRIPTOR host_sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &host_sd, nullptr)) return 25;
  SECURITY_ATTRIBUTES host_sa{sizeof(host_sa), host_sd, FALSE};
  Handle host;
  host.h = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 512, 512, 0, &host_sa);
  LocalFree(host_sd);
  if (!host.valid()) return 26;
  // Nonblocking listening state avoids an outstanding OVERLAPPED at cleanup.
  if (ConnectNamedPipe(host.h, nullptr) || GetLastError() != ERROR_PIPE_LISTENING) return 27;
  {
    Handle allowed;
    allowed.h = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!allowed.valid()) return 28;
  }
  if (!DisconnectNamedPipe(host.h)) return 30;
  if (ConnectNamedPipe(host.h, nullptr) || GetLastError() != ERROR_PIPE_LISTENING) return 31;
  Handle in_read, in_write, out_read, out_write;
  SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
  if (!CreatePipe(&in_read.h, &in_write.h, &inherited, 0) || !CreatePipe(&out_read.h, &out_write.h, &inherited, 0) ||
      !SetHandleInformation(in_write.h, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0) ||
      !write_bytes(in_write.h, "Gesher-resource-v1\n", 19)) return 32;
  in_write.reset();
  Handle job;
  job.h = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  if (!job.valid() || !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 33;
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 3, 0, &bytes);
  std::vector<BYTE> storage(bytes);
  auto attrs = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attrs, 3, 0, &bytes)) return 34;
  SECURITY_CAPABILITIES caps{}; caps.AppContainerSid = f.sid;
  HANDLE handles[]{in_read.h, out_write.h};
  bool configured = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr) &&
      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) &&
      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.h, sizeof(job.h), nullptr, nullptr);
  std::wstring cmd = L"\"" + f.executable + L"\" child " + sid_text + L" " + std::to_wstring(GetCurrentProcessId()) +
      L" " + std::to_wstring(GetCurrentThreadId()) + L" \"" + f.code + L"\" \"" + f.config + L"\" \"" + pipe_name + L"\" " +
      std::to_wstring(reinterpret_cast<ULONG_PTR>(in_read.h)) + L" " + std::to_wstring(reinterpret_cast<ULONG_PTR>(out_write.h));
  STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi{};
  BOOL started = configured && CreateProcessW(f.executable.c_str(), cmd.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, f.root.c_str(), &si.StartupInfo, &pi);
  const DWORD start_error = GetLastError();
  DeleteProcThreadAttributeList(attrs);
  if (!started) { fprintf(stderr, "launch error=%lu\n", start_error); return 35; }
  Handle process, thread; process.h = pi.hProcess; thread.h = pi.hThread;
  struct OwnedChild {
    HANDLE process, job; bool& stopped;
    ~OwnedChild() {
      if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
        if (!TerminateJobObject(job, 90)) fprintf(stderr, "owned job termination failed error=%lu\n", GetLastError());
      }
      stopped = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
      if (!stopped) fprintf(stderr, "owned process not signaled after cleanup deadline\n");
    }
  } owned{process.h, job.h, f.child_stopped};
  f.child_stopped = false;
  in_read.reset(); out_write.reset();
  BOOL in_job = FALSE;
  if (!IsProcessInJob(process.h, job.h, &in_job) || !in_job || !token_is(process.h, f.sid)) return 36;
  printf("PASS parent suspended-token-sid pid=%lu tid=%lu owned-job=1\n", pi.dwProcessId, pi.dwThreadId);
  if (ResumeThread(thread.h) == static_cast<DWORD>(-1)) return 37;
  if (WaitForSingleObject(process.h, 15000) != WAIT_OBJECT_0) return 38;
  DWORD code = 99;
  if (!GetExitCodeProcess(process.h, &code) || code != 0) { fprintf(stderr, "child exit=%lu\n", code); return 39; }
  // Peek first: never make a blocking read against a retained pipe writer.
  DWORD available = 0;
  if (!PeekNamedPipe(out_read.h, nullptr, 0, nullptr, &available, nullptr) || available != sizeof(kRows)-1) return 40;
  char rows[sizeof(kRows)]{}; DWORD read = 0;
  if (!ReadFile(out_read.h, rows, available, &read, nullptr) || read != available || memcmp(rows, kRows, available) != 0) return 41;
  printf("%s", rows);
  if (!unchanged(f.code) || !unchanged(f.config)) return 42;
  printf("PASS parent trusted-files-unchanged host-ipc-positive=1 child-exit=0\n");
  return 0;
}
int wmain(int argc, wchar_t** argv) {
  if (argc > 1 && wcscmp(argv[1], L"child") == 0) return child(argc, argv);
  Fixture fixture;
  const int result = exercise(fixture);
  if (!fixture.cleanup()) { fprintf(stderr, "owned cleanup failed\n"); return 50; }
  if (result) { fprintf(stderr, "probe stage=%d winerror=%lu\n", result, GetLastError()); return result; }
  puts("PASS parent owned-child-handles-files-profile-cleanup");
  puts("BOUNDARY disposable-AppContainer-only trusted-same-user-host-outside-scope");
  return 0;
}
