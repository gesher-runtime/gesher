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
// Fixed-size observation precedes the unchanged success rows on the existing
// explicit output pipe. Parent prints it even when the strict child check fails.
struct IpcObservation {
  ULONG_PTR handle;
  DWORD error, requested, server_pid;
};
static void print_sid(const char* field, PSID sid, DWORD attributes) {
  LPWSTR text = nullptr;
  if (sid && ConvertSidToStringSidW(sid, &text)) {
    printf("DIAG token %s sid=%ls attributes=0x%08lx\n", field, text, attributes);
    LocalFree(text);
  } else printf("DIAG token %s sid-query-error=%lu\n", field, GetLastError());
}
static bool token_evidence(HANDLE process) {
  Handle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token.h)) return false;
  for (auto kind : {TokenUser, TokenAppContainerSid, TokenIntegrityLevel, TokenGroups, TokenRestrictedSids, TokenCapabilities}) {
    DWORD size = 0;
    GetTokenInformation(token.h, kind, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    if (!size || !GetTokenInformation(token.h, kind, data.data(), size, &size)) return false;
    if (kind == TokenUser) {
      const auto* user = reinterpret_cast<TOKEN_USER*>(data.data());
      print_sid("user", user->User.Sid, user->User.Attributes);
    } else if (kind == TokenAppContainerSid) {
      const auto* app = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(data.data());
      print_sid("appcontainer", app->TokenAppContainer, 0);
    } else if (kind == TokenIntegrityLevel) {
      const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data());
      print_sid("integrity", label->Label.Sid, label->Label.Attributes);
    } else {
      const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(data.data());
      const char* name = kind == TokenGroups ? "group" : kind == TokenRestrictedSids ? "restricted" : "capability";
      printf("DIAG token %s count=%lu\n", name, groups->GroupCount);
      for (DWORD i = 0; i < groups->GroupCount; ++i)
        print_sid(name, groups->Groups[i].Sid, groups->Groups[i].Attributes);
    }
  }
  return true;
}
static bool pipe_evidence(HANDLE pipe, PSID allowed) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  const SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
  const DWORD error = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, fields, nullptr, nullptr, nullptr, nullptr, &sd);
  if (error != ERROR_SUCCESS) { printf("DIAG host-ipc security-query-error=%lu\n", error); return false; }
  LPWSTR text = nullptr;
  bool ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, fields, &text, nullptr) != FALSE;
  if (ok) { printf("DIAG host-ipc actual-owner-group-dacl-label=%ls\n", text); LocalFree(text); }
  PACL dacl = nullptr, sacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
  PSID owner_rights = nullptr, low = nullptr;
  ok = ok && ConvertStringSidToSidW(L"S-1-3-4", &owner_rights) && ConvertStringSidToSidW(L"S-1-16-4096", &low) &&
      GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED) &&
      GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present && dacl && dacl->AceCount == (allowed ? 2 : 1);
  GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
  for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    ok = GetAce(dacl, i, &raw) != FALSE;
    if (!ok) break;
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    DWORD mask = ace->Mask; MapGenericMask(&mask, &mapping);
    ok = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
        EqualSid(const_cast<DWORD*>(&ace->SidStart), i == 0 ? owner_rights : allowed) &&
        mask == (i == 0 ? FILE_ALL_ACCESS : FILE_GENERIC_READ | FILE_GENERIC_WRITE);
  }
  ok = ok && GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present && sacl && sacl->AceCount == 1;
  if (ok) {
    void* raw = nullptr; ok = GetAce(sacl, 0, &raw) != FALSE;
    if (ok) {
      const auto* label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(raw);
      ok = label->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE && label->Header.AceFlags == 0 &&
          label->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP && EqualSid(const_cast<DWORD*>(&label->SidStart), low);
    }
  }
  if (owner_rights) LocalFree(owner_rights);
  if (low) LocalFree(low);
  LocalFree(sd);
  return ok;
}
static bool read_observation(HANDLE input, HANDLE process, IpcObservation* value) {
  const ULONGLONG deadline = GetTickCount64() + 5000;
  do {
    DWORD available = 0, read = 0;
    if (PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) && available >= sizeof(*value))
      return ReadFile(input, value, sizeof(*value), &read, nullptr) && read == sizeof(*value);
    if (WaitForSingleObject(process, 5) != WAIT_TIMEOUT) return false;
  } while (GetTickCount64() < deadline);
  return false;
}
static bool protect_pipe(HANDLE pipe) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) return false;
  PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
      SetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                      nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
  LocalFree(sd);
  return ok;
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
  Handle input, output;
  input.h = reinterpret_cast<HANDLE>(_wcstoui64(argv[8], nullptr, 10));
  output.h = reinterpret_cast<HANDLE>(_wcstoui64(argv[9], nullptr, 10));
  char challenge[20]{}; DWORD n = 0;
  if (!ReadFile(input.h, challenge, sizeof(challenge)-1, &n, nullptr) || n != 19 ||
      memcmp(challenge, "Gesher-resource-v1\n", 19) != 0) return 18;
  // Prove a fresh connection to the very same host endpoint before its DACL
  // is tightened. A missing or rewritten namespace cannot pass this control.
  SetLastError(ERROR_SUCCESS);
  Handle positive;
  positive.h = CreateFileW(argv[7], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD positive_error = GetLastError();
  ULONG server_pid = 0;
  const bool endpoint = positive.valid() && GetNamedPipeServerProcessId(positive.h, &server_pid) && server_pid == pid &&
      write_bytes(positive.h, challenge, 19);
  const IpcObservation positive_observation{reinterpret_cast<ULONG_PTR>(positive.h), positive_error,
      GENERIC_READ | GENERIC_WRITE, server_pid};
  positive.reset();
  if (!write_bytes(output.h, reinterpret_cast<const char*>(&positive_observation), sizeof(positive_observation))) return 53;
  if (!endpoint) return 57;
  char protected_ack = 0;
  if (!ReadFile(input.h, &protected_ack, 1, &n, nullptr) || n != 1 || protected_ack != 'D') return 58;
  SetLastError(ERROR_SUCCESS);
  HANDLE opened = CreateFileW(argv[7], GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD open_error = GetLastError();  // Capture before any logging/close.
  const IpcObservation observation{reinterpret_cast<ULONG_PTR>(opened), open_error, GENERIC_READ | GENERIC_WRITE, 0};
  const bool ipc_denied = opened == INVALID_HANDLE_VALUE && open_error == ERROR_ACCESS_DENIED;
  if (opened != nullptr && opened != INVALID_HANDLE_VALUE) CloseHandle(opened);
  if (!write_bytes(output.h, reinterpret_cast<const char*>(&observation), sizeof(observation))) return 53;
  if (!ipc_denied) return 17;
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
  // Unpackaged Win32 host endpoint. LOCAL was unreachable in the AppContainer:
  // actual c2d667ea CI40783823671 returned FILE_NOT_FOUND on both runners.
  const std::wstring pipe_name = L"\\\\.\\pipe\\" + f.profile;
  PSECURITY_DESCRIPTOR host_sd = nullptr;
  const std::wstring positive_acl = L"D:P(A;;GA;;;OW)(A;;GRGW;;;" + sid_text + L")S:(ML;;NW;;;LW)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(positive_acl.c_str(), SDDL_REVISION_1, &host_sd, nullptr)) return 25;
  SECURITY_ATTRIBUTES host_sa{sizeof(host_sa), host_sd, FALSE};
  Handle host;
  host.h = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 512, 512, 0, &host_sa);
  LocalFree(host_sd);
  if (!host.valid()) return 26;
  if (!pipe_evidence(host.h, f.sid)) return 54;
  // Nonblocking listening state avoids an outstanding OVERLAPPED at cleanup.
  if (ConnectNamedPipe(host.h, nullptr) || GetLastError() != ERROR_PIPE_LISTENING) return 27;
  {
    Handle allowed;
    SetLastError(ERROR_SUCCESS);
    allowed.h = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD allowed_error = GetLastError();
    printf("DIAG parent host-ipc-open handle=%p valid=%d error=%lu\n", allowed.h, allowed.valid(), allowed_error);
    if (!allowed.valid()) return 28;
  }
  if (!DisconnectNamedPipe(host.h)) return 30;
  if (ConnectNamedPipe(host.h, nullptr) || GetLastError() != ERROR_PIPE_LISTENING) return 31;
  Handle in_read, in_write, out_read, out_write;
  SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
  if (!CreatePipe(&in_read.h, &in_write.h, &inherited, 0) || !CreatePipe(&out_read.h, &out_write.h, &inherited, 0) ||
      !SetHandleInformation(in_write.h, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0) ||
      !write_bytes(in_write.h, "Gesher-resource-v1\n", 19)) return 32;
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
  printf("DIAG host-ipc path=%ls requested=0x%08lx share=0 creation=OPEN_EXISTING flags=0\n", pipe_name.c_str(), static_cast<DWORD>(GENERIC_READ | GENERIC_WRITE));
  if (!token_evidence(process.h)) return 55;
  if (ResumeThread(thread.h) == static_cast<DWORD>(-1)) return 37;
  IpcObservation positive_observation{};
  if (!read_observation(out_read.h, process.h, &positive_observation)) return 59;
  printf("DIAG child host-ipc-positive handle=%p error=%lu requested=0x%08lx server-pid=%lu\n",
      reinterpret_cast<void*>(positive_observation.handle), positive_observation.error,
      positive_observation.requested, positive_observation.server_pid);
  if (!positive_observation.handle || positive_observation.handle == reinterpret_cast<ULONG_PTR>(INVALID_HANDLE_VALUE) ||
      positive_observation.requested != (GENERIC_READ | GENERIC_WRITE) || positive_observation.server_pid != GetCurrentProcessId()) return 60;
  char actual_challenge[20]{}; DWORD challenge_available = 0, challenge_read = 0;
  if (!PeekNamedPipe(host.h, nullptr, 0, nullptr, &challenge_available, nullptr) || challenge_available != 19 ||
      !ReadFile(host.h, actual_challenge, 19, &challenge_read, nullptr) || challenge_read != 19 ||
      memcmp(actual_challenge, "Gesher-resource-v1\n", 19) != 0) return 61;
  if (!DisconnectNamedPipe(host.h) || !protect_pipe(host.h) || !pipe_evidence(host.h, nullptr)) return 62;
  if (ConnectNamedPipe(host.h, nullptr) || GetLastError() != ERROR_PIPE_LISTENING) return 63;
  {
    Handle fresh_host;
    fresh_host.h = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!fresh_host.valid()) return 64;
  }
  if (!DisconnectNamedPipe(host.h)) return 65;
  if (ConnectNamedPipe(host.h, nullptr) || GetLastError() != ERROR_PIPE_LISTENING) return 66;
  puts("PASS parent same-endpoint-isolated-positive-challenge fresh-host-positive protected-dacl");
  if (!write_bytes(in_write.h, "D", 1)) return 67;
  in_write.reset();
  if (WaitForSingleObject(process.h, 15000) != WAIT_OBJECT_0) return 38;
  DWORD code = 99;
  if (!GetExitCodeProcess(process.h, &code)) return 39;
  DWORD observation_available = 0;
  IpcObservation observation{}; DWORD observation_read = 0;
  const bool observed = PeekNamedPipe(out_read.h, nullptr, 0, nullptr, &observation_available, nullptr) &&
      observation_available >= sizeof(observation) &&
      ReadFile(out_read.h, &observation, sizeof(observation), &observation_read, nullptr) && observation_read == sizeof(observation);
  if (observed) printf("DIAG child host-ipc-open handle=%p invalid=%d error=%lu requested=0x%08lx expected-error=5\n",
      reinterpret_cast<void*>(observation.handle), observation.handle == reinterpret_cast<ULONG_PTR>(INVALID_HANDLE_VALUE),
      observation.error, observation.requested);
  else printf("DIAG child host-ipc-open observation-missing exit=%lu available=%lu\n", code, observation_available);
  if (code != 0) { fprintf(stderr, "child exit=%lu\n", code); return 39; }
  if (!observed || observation.handle != reinterpret_cast<ULONG_PTR>(INVALID_HANDLE_VALUE) ||
      observation.error != ERROR_ACCESS_DENIED || observation.requested != (GENERIC_READ | GENERIC_WRITE)) return 56;
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
