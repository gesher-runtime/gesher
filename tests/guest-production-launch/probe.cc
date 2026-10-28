// Shared restricted launch API boundary fixture. Production adoption remains open.
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
#include "../../compat/framework/restricted_launch.h"

// A failed cleanup destructor must not allocate even when allocation is fatal.
static bool g_forbid_allocations = false;
void* operator new(size_t size) {
  if (g_forbid_allocations) abort();
  void* result = malloc(size ? size : 1);
  if (!result) abort();
  return result;
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { free(pointer); }
void operator delete[](void* pointer) noexcept { free(pointer); }
void operator delete(void* pointer, size_t) noexcept { free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { free(pointer); }

namespace gesher::guest {
struct RestrictedLaunchTestAccess {
  static bool ExhaustionRejects(const wchar_t* executable, wchar_t* cmd, const wchar_t* directory,
                               const STARTUPINFOW& startup, PSID sid, const std::vector<HANDLE>& allowlist) {
    std::array<RestrictedLaunch::Pending*, 64> reservations{};
    for (auto& reservation : reservations) {
      reservation = RestrictedLaunch::Reserve();
      if (!reservation) return false;
    }
    RestrictedLaunch rejected;
    const auto result = rejected.Start(executable, cmd, nullptr, directory, startup, 0, sid, allowlist);
    bool ok = result.error == ERROR_TOO_MANY_CMDS && result.cleanup.confirmed && !rejected.process();
    for (auto* reservation : reservations) reservation->state.store(0, std::memory_order_release);
    return ok;
  }
  static bool BusyRetryPreservesUnknown() {
    for (auto& slot : RestrictedLaunch::Slots()) {
      unsigned expected = 2;
      if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) continue;
      bool ok = RestrictedLaunch::RetryQuarantined() == 1;
      auto evidence = RestrictedLaunch::PendingRetirements();
      ok = ok && evidence.size() == 1 && !evidence[0].confirmed && evidence[0].wait_error == ERROR_BUSY;
      slot.state.store(2, std::memory_order_release);
      return ok;
    }
    return false;
  }
  static BOOL WINAPI DenyTermination(HANDLE, UINT) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  static DWORD WINAPI FailWait(HANDLE, DWORD) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
  static DWORD WINAPI FailResume(HANDLE) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return static_cast<DWORD>(-1); }
  static void Inject(RestrictedLaunch& owner, bool failing) {
    owner.resume_ = failing ? FailResume : ResumeThread;
    owner.terminate_ = failing ? DenyTermination : TerminateJobObject;
    owner.wait_ = failing ? FailWait : WaitForSingleObject;
  }
};
}

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
  printf("DIAG host-ipc descriptor-assertion phase=%s success=%d\n", allowed ? "allowed" : "protected", ok);
  LocalFree(sd);
  return ok;
}
static bool read_observation(HANDLE input, HANDLE process, IpcObservation* value) {
  const ULONGLONG deadline = GetTickCount64() + 5000;
  DWORD available = 0, peek_error = ERROR_SUCCESS, state = WAIT_TIMEOUT;
  do {
    DWORD read = 0;
    const BOOL peeked = PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr);
    peek_error = peeked ? ERROR_SUCCESS : GetLastError();
    if (peeked && available >= sizeof(*value)) {
      const BOOL read_ok = ReadFile(input, value, sizeof(*value), &read, nullptr);
      const DWORD read_error = read_ok ? ERROR_SUCCESS : GetLastError();
      if (read_ok && read == sizeof(*value)) return true;
      printf("DIAG observation read-success=%d read-error=%lu bytes=%lu expected=%zu\n",
          read_ok, read_error, read, sizeof(*value));
      break;
    }
    state = WaitForSingleObject(process, 5);
    if (state != WAIT_TIMEOUT) break;
  } while (GetTickCount64() < deadline);
  const DWORD wait_error = state == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  DWORD exit_code = 0;
  const BOOL queried = GetExitCodeProcess(process, &exit_code);
  const DWORD exit_error = queried ? ERROR_SUCCESS : GetLastError();
  printf("DIAG observation missing available=%lu peek-error=%lu wait=%lu wait-error=%lu exit-query=%d exit=%lu exit-error=%lu\n",
      available, peek_error, state, wait_error, queried, exit_code, exit_error);
  return false;
}
static bool protect_pipe(HANDLE pipe) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) {
    printf("DIAG host-ipc protect-sddl error=%lu\n", GetLastError()); return false;
  }
  PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present;
  if (ok) {
    const DWORD status = SetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, dacl, nullptr);
    printf("DIAG host-ipc protect-SetSecurityInfo status=%lu\n", status);
    ok = status == ERROR_SUCCESS;
  } else printf("DIAG host-ipc protect-GetSecurityDescriptorDacl error=%lu present=%d\n", GetLastError(), present);
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
  if (argc != 11) return 10;
  HANDLE omitted = reinterpret_cast<HANDLE>(_wcstoui64(argv[10], nullptr, 10));
  SetLastError(ERROR_SUCCESS);
  if (SetEvent(omitted) || GetLastError() != ERROR_INVALID_HANDLE) return 72;
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
  host.h = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | WRITE_DAC | FILE_FLAG_FIRST_PIPE_INSTANCE,
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
  Handle omitted;
  omitted.h = CreateEventW(&inherited, TRUE, FALSE, nullptr);
  if (!omitted.valid()) return 73;
  HANDLE handles[]{in_read.h, out_write.h};
  std::wstring cmd = L"\"" + f.executable + L"\" child " + sid_text + L" " + std::to_wstring(GetCurrentProcessId()) +
      L" " + std::to_wstring(GetCurrentThreadId()) + L" \"" + f.code + L"\" \"" + f.config + L"\" \"" + pipe_name + L"\" " +
      std::to_wstring(reinterpret_cast<ULONG_PTR>(in_read.h)) + L" " + std::to_wstring(reinterpret_cast<ULONG_PTR>(out_write.h)) + L" " + std::to_wstring(reinterpret_cast<ULONG_PTR>(omitted.h));
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  const std::vector<HANDLE> allowlist{handles[0], handles[1]};
  {
    std::wstring rejected_cmd = cmd;
    if (!gesher::guest::RestrictedLaunchTestAccess::ExhaustionRejects(f.executable.c_str(),
        rejected_cmd.data(), f.root.c_str(), startup, f.sid, allowlist)) return 80;
  }
  {
    gesher::guest::RestrictedLaunch invalid;
    std::wstring bad = cmd;
    if (invalid.Start(f.executable.c_str(), bad.data(), nullptr, f.root.c_str(), startup,
                      CREATE_BREAKAWAY_FROM_JOB, f.sid, allowlist).error != ERROR_INVALID_PARAMETER ||
        invalid.process() || invalid.Resume().error != ERROR_INVALID_STATE || !invalid.Stop().confirmed) return 70;
  }
  {
    gesher::guest::RestrictedLaunch rollback;
    std::wstring rollback_cmd = cmd;
    if (rollback.Start(f.executable.c_str(), rollback_cmd.data(), nullptr, f.root.c_str(), startup,
                       0, f.sid, allowlist).error != ERROR_SUCCESS || !rollback.process() ||
        !rollback.Stop().confirmed || WaitForSingleObject(rollback.process(), 0) != WAIT_OBJECT_0 ||
        rollback.Resume().error != ERROR_INVALID_STATE) return 71;
  }
  {
    gesher::guest::RestrictedLaunch rollback;
    std::wstring rollback_cmd = cmd;
    if (rollback.Start(f.executable.c_str(), rollback_cmd.data(), nullptr, f.root.c_str(), startup,
                       0, f.sid, allowlist).error != ERROR_SUCCESS) return 75;
    HANDLE exact_process = rollback.process(), exact_job = rollback.job();
    gesher::guest::RestrictedLaunchTestAccess::Inject(rollback, true);
    const auto rejected = rollback.Resume();
    const auto failed = rejected.cleanup;
    if (rejected.error != ERROR_NOT_ENOUGH_MEMORY || failed.confirmed || failed.terminate_error != ERROR_ACCESS_DENIED ||
        failed.wait_error != ERROR_INVALID_HANDLE || rollback.process() != exact_process ||
        rollback.job() != exact_job || WaitForSingleObject(exact_process, 0) != WAIT_TIMEOUT) return 76;
    gesher::guest::RestrictedLaunchTestAccess::Inject(rollback, false);
    if (!rollback.Stop().confirmed || WaitForSingleObject(exact_process, 0) != WAIT_OBJECT_0) return 77;
  }
  {
    gesher::guest::RestrictedLaunch abandoned;
    std::wstring rollback_cmd = cmd;
    if (abandoned.Start(f.executable.c_str(), rollback_cmd.data(), nullptr, f.root.c_str(), startup,
                        0, f.sid, allowlist).error != ERROR_SUCCESS) return 78;
    gesher::guest::RestrictedLaunchTestAccess::Inject(abandoned, true);
    g_forbid_allocations = true;
  }
  g_forbid_allocations = false;
  if (!gesher::guest::RestrictedLaunchTestAccess::BusyRetryPreservesUnknown()) return 81;
  const auto pending = gesher::guest::RestrictedLaunch::PendingRetirements();
  if (pending.size() != 1 || pending[0].confirmed || pending[0].terminate_error != ERROR_ACCESS_DENIED ||
      pending[0].wait_error != ERROR_INVALID_HANDLE || gesher::guest::RestrictedLaunch::RetryQuarantined() != 0 ||
      !gesher::guest::RestrictedLaunch::PendingRetirements().empty()) return 79;
  puts("PASS shared-api bounded-admission-allocation-free-handoff-busy-retry-unknown");
  puts("PASS shared-api unconfirmed-retirement-evidence-owned-retry-quarantine");
  puts("PASS shared-api forbidden-flags-fail-closed suspended-owned-rollback");
  gesher::guest::RestrictedLaunch launch;
  const auto start = launch.Start(f.executable.c_str(), cmd.data(), nullptr,
      f.root.c_str(), startup, 0, f.sid, allowlist);
  if (start.error != ERROR_SUCCESS || !start.cleanup.confirmed) { fprintf(stderr, "launch error=%lu cleanup=%d terminate-error=%lu wait-error=%lu\n", start.error, start.cleanup.confirmed, start.cleanup.terminate_error, start.cleanup.wait_error); return 35; }
  struct Borrowed { HANDLE h; } process{launch.process()}, job{launch.job()};
  struct OwnedChild {
    gesher::guest::RestrictedLaunch& launch; bool& stopped;
    ~OwnedChild() {
      const auto retired = launch.Stop();
      stopped = retired.confirmed;
      DWORD code = 0;
      const BOOL queried = GetExitCodeProcess(launch.process(), &code);
      const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
      printf("DIAG owned-child retirement confirmed=%d terminate-error=%lu wait-error=%lu exit-query=%d exit=%lu exit-error=%lu\n",
          stopped, retired.terminate_error, retired.wait_error, queried, code, error);
      if (!stopped) fprintf(stderr, "owned process not signaled after cleanup deadline\n");
    }
  } owned{launch, f.child_stopped};
  f.child_stopped = false;
  in_read.reset(); out_write.reset();
  BOOL in_job = FALSE;
  if (!IsProcessInJob(process.h, job.h, &in_job) || !in_job || !token_is(process.h, f.sid)) return 36;
  printf("PASS parent suspended-token-sid pid=%lu tid=%lu owned-job=1\n", launch.pid(), launch.tid());
  printf("DIAG host-ipc path=%ls requested=0x%08lx share=0 creation=OPEN_EXISTING flags=0\n", pipe_name.c_str(), static_cast<DWORD>(GENERIC_READ | GENERIC_WRITE));
  if (!token_evidence(process.h)) return 55;
  if (launch.Resume().error != ERROR_SUCCESS || launch.Resume().error != ERROR_INVALID_STATE) return 37;
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
  const BOOL disconnected_positive = DisconnectNamedPipe(host.h);
  const DWORD disconnect_error = disconnected_positive ? ERROR_SUCCESS : GetLastError();
  printf("DIAG host-ipc positive-disconnect success=%d error=%lu\n", disconnected_positive, disconnect_error);
  if (!disconnected_positive || !protect_pipe(host.h) || !pipe_evidence(host.h, nullptr)) return 62;
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
  if (WaitForSingleObject(omitted.h, 0) != WAIT_TIMEOUT) return 74;
  puts("PASS shared-api unlisted-inheritable-event-denied");
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
  puts("BOUNDARY shared-restricted-launch-only production-SpawnOwned-adoption-open trusted-same-user-host-outside-scope");
  return 0;
}
