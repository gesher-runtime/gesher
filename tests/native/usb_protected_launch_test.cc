// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Elevated, disposable Windows contract for the actual private identity and
// lowbox launcher. Internal service/guest modes exist only in this test image.
#include "../../compat/usb_storage/protected_identity.h"
#include "../../compat/usb_storage/authority_launch.h"
#include "../../compat/framework/app_container.h"
#include "usb_scm_profile_control.h"
#include <aclapi.h>
#include <securityappcontainer.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <winternl.h>
#include <wtsapi32.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace storage = gesher::usb_storage;
namespace container = gesher::appcontainer;
namespace profile_control = gesher::usb_storage::test;
struct Close { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct ScClose { void operator()(SC_HANDLE h) const { if (h) CloseServiceHandle(h); } };
struct Free { void operator()(void* p) const { if (p) LocalFree(p); } };
using Handle = std::unique_ptr<void, Close>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScClose>;
using Local = std::unique_ptr<void, Free>;
struct Failure { DWORD status; const char* stage; };
void Require(bool ok, const char* stage, DWORD status = ERROR_SUCCESS) {
  if (!ok) {
    const DWORD error = status ? status : GetLastError();
    throw Failure{error ? error : ERROR_GEN_FAILURE, stage};
  }
}
void Check(DWORD status, const char* stage) { Require(status == ERROR_SUCCESS, stage, status); }
struct ProfileDeletionEvidence {
  DWORD attempts = 0;
  DWORD api_status = ERROR_NOT_READY;
  DWORD hive_status = ERROR_NOT_READY;
};
enum class Trustee : DWORD { unknown, private_user, system, administrators, owner_rights, everyone, authenticated, logon, other, private_logon };
struct TokenAceEvidence { DWORD type = 0, flags = 0, mask = 0; Trustee trustee = Trustee::unknown; };
struct TokenObjectEvidence {
  DWORD status = ERROR_NOT_READY;
  Trustee owner = Trustee::unknown;
  DWORD control = 0, present = 0, defaulted = 0, null_acl = 0, count = 0, recorded = 0;
  TokenAceEvidence aces[12]{};
};
struct TokenCallEvidence {
  DWORD called = 0, status = ERROR_NOT_READY, succeeded = 0, returned_token = 0;
};
struct TokenPhaseEvidence {
  TokenObjectEvidence object;
  DWORD impersonate = ERROR_NOT_READY;
  TokenCallEvidence thread_query_private, thread_query_self, thread_combined_private, thread_combined_self;
  TokenCallEvidence duplicate_launch, duplicate_all, wrapper, native_private, native_system;
};
constexpr size_t kNamespacePathCharacters = 384;
struct NamespacePathEvidence {
  DWORD status = ERROR_NOT_READY, succeeded = 0, returned_length = 0, validation = ERROR_NOT_READY;
  wchar_t path[kNamespacePathCharacters]{};
};
struct DirectoryCallEvidence { DWORD called = 0, status = 0, returned_handle = 0; };
struct DirectoryAccessEvidence {
  DWORD setup_status = ERROR_NOT_READY, called = 0, status = ERROR_NOT_READY, authorized = 0, granted = 0;
};
struct NamespaceDirectoryEvidence {
  // Bits: 1=absolute API result, 2=worker-session candidate, 4=token-session
  // candidate. Shared ancestors retain every applicable candidate bit.
  DWORD candidates = 0, depth = 0, impersonate = ERROR_NOT_READY;
  wchar_t path[kNamespacePathCharacters]{};
  DirectoryCallEvidence system_query, system_security, private_query, private_security;
  DirectoryAccessEvidence private_create_subdirectory;
  TokenObjectEvidence security;
};
struct NamespaceEvidence {
  DWORD token_validation = ERROR_NOT_READY;
  DWORD private_logon_status = ERROR_NOT_READY, access_context_status = ERROR_NOT_READY;
  DWORD status = ERROR_NOT_READY, worker_session = 0, token_session = 0, private_impersonate = ERROR_NOT_READY;
  DWORD directory_export = ERROR_NOT_READY, source_private = 0, count = 0;
  NamespacePathEvidence system_path, private_path;
  NamespaceDirectoryEvidence directories[8]{};
};
struct EquivalentTokenEvidence {
  DWORD attempted = 0, status = ERROR_NOT_READY;
  DWORD wrapper_export = ERROR_NOT_READY, native_export = ERROR_NOT_READY;
  TokenPhaseEvidence before, after;
  NamespaceEvidence object_namespace;
  char stage[64] = "not-attempted";
};
struct Evidence {
  DWORD magic = 0x47504c54;
  DWORD status = ERROR_SUCCESS;
  DWORD cleanup = ERROR_SUCCESS;
  DWORD image_access = ERROR_NOT_READY;
  DWORD directory_access = ERROR_NOT_READY;
  DWORD target_token_access = ERROR_NOT_READY;
  DWORD private_caller_token_access = ERROR_NOT_READY;
  DWORD system_caller_token_access = ERROR_NOT_READY;
  EquivalentTokenEvidence equivalent;
  profile_control::ScmProfileControlEvidence scm_control;
  char stage[96] = "complete";
  char cleanup_stage[64] = "none";
};
// Fixed diagnostic payload: two API paths, eight exact directory observations,
// and at most twelve ACEs per object. The controller validates counts/strings.
static_assert(std::is_trivially_copyable_v<Evidence> && sizeof(Evidence) <= 16384);
struct Configuration {
  std::wstring name, image, directory;
  DWORD session = 0;
  ULONGLONG worker_deadline = 0;
  bool scm_profile_control = false;
} config;
SERVICE_STATUS_HANDLE service_status = nullptr;
Handle service_stop;
Handle service_job;

std::wstring Image() {
  std::array<wchar_t, 32768> path{};
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  Require(size && size < path.size(), "test-image-path");
  return {path.data(), size};
}
std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }
bool DeadlineArgument(std::wstring_view value, ULONGLONG* result) {
  if (!result || value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == L'0')) return false;
  ULONGLONG parsed = 0;
  for (wchar_t c : value) {
    if (c < L'0' || c > L'9') return false;
    const ULONGLONG digit = static_cast<ULONGLONG>(c - L'0');
    if (parsed > ((std::numeric_limits<ULONGLONG>::max)() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  *result = parsed;
  return true;
}
Local Security(bool low = false) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  const wchar_t* policy = low ? L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)S:(ML;OICI;NW;;;LW)" :
      L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      policy, SDDL_REVISION_1, &raw, nullptr), "fixture-security");
  return Local(raw);
}
void PrivateDirectory(const std::wstring& path, bool low = false) {
  const Local descriptor = Security(low);
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  Require(CreateDirectoryW(path.c_str(), &security), "fixture-directory-create");
}
void WriteNew(const std::wstring& path, const void* data, DWORD bytes) {
  const Local descriptor = Security();
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
      CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Require(file.get() != INVALID_HANDLE_VALUE, "fixture-file-create");
  DWORD written = 0;
  Require(WriteFile(file.get(), data, bytes, &written, nullptr) && written == bytes, "fixture-file-write");
}
std::vector<BYTE> Read(const std::wstring& path, DWORD maximum) {
  const Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Require(file.get() != INVALID_HANDLE_VALUE, "fixture-file-read-open");
  BY_HANDLE_FILE_INFORMATION info{};
  Require(GetFileInformationByHandle(file.get(), &info) && info.nNumberOfLinks == 1 &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
      info.nFileSizeHigh == 0 && info.nFileSizeLow <= maximum, "fixture-file-read-type", ERROR_INVALID_DATA);
  std::vector<BYTE> bytes(info.nFileSizeLow);
  DWORD count = 0;
  Require(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
      count == bytes.size(), "fixture-file-read");
  return bytes;
}
std::vector<BYTE> User(HANDLE token) {
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  Require(GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &bytes), "token-user");
  PSID sid = reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid;
  std::vector<BYTE> result(GetLengthSid(sid));
  Require(CopySid(static_cast<DWORD>(result.size()), result.data(), sid), "copy-token-user");
  return result;
}
std::vector<BYTE> RuntimeSid() {
  DWORD bytes = SECURITY_MAX_SID_SIZE, domain_bytes = 256;
  std::vector<BYTE> result(bytes);
  wchar_t domain[256]{};
  SID_NAME_USE use{};
  Require(LookupAccountNameW(nullptr, storage::kRuntimeIdentityAccount, result.data(), &bytes,
      domain, &domain_bytes, &use), "lookup-runtime-sid");
  Require(IsValidSid(result.data()), "validate-runtime-sid", ERROR_INVALID_SID);
  // cbSid is an input capacity; its documented output is the required length
  // on an insufficient buffer. Serialize the validated SID, not spare capacity.
  const DWORD length = GetLengthSid(result.data());
  Require(length >= 8 && length <= result.size(), "runtime-sid-length", ERROR_INVALID_SID);
  result.resize(length);
  return result;
}
std::wstring SidText(PSID sid) {
  LPWSTR raw = nullptr;
  Require(ConvertSidToStringSidW(sid, &raw), "sid-string");
  const Local own(raw);
  return raw;
}
void AbsentRuntimeService() {
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  Require(!!manager, "open-scm");
  const Service existing(OpenServiceW(manager.get(), storage::kRuntimeIdentityService, SERVICE_QUERY_STATUS));
  Require(!existing && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST,
      "refuse-preexisting-runtime-service", ERROR_ALREADY_EXISTS);
}
void AbsentRuntimeProfile(PSID sid) {
  const std::wstring path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + SidText(sid);
  HKEY key = nullptr;
  const LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
  if (key) RegCloseKey(key);
  Require(result == ERROR_FILE_NOT_FOUND, "refuse-preexisting-runtime-profile",
      result == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : static_cast<DWORD>(result));
}
DWORD DeleteOwnedProfile(const std::wstring& directory, ProfileDeletionEvidence* observed) {
  const std::wstring marker = directory + L"\\profile.sid";
  if (GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
  }
  try {
    const auto sid = Read(marker, SECURITY_MAX_SID_SIZE);
    Require(sid.size() >= 8 && IsValidSid(const_cast<BYTE*>(sid.data())) &&
        GetLengthSid(const_cast<BYTE*>(sid.data())) == sid.size(), "owned-profile-marker", ERROR_INVALID_SID);
    const SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    Require(*GetSidSubAuthorityCount(const_cast<BYTE*>(sid.data())) == 6 &&
        std::memcmp(GetSidIdentifierAuthority(const_cast<BYTE*>(sid.data())), &nt, sizeof(nt)) == 0 &&
        *GetSidSubAuthority(const_cast<BYTE*>(sid.data()), 0) == SECURITY_SERVICE_ID_BASE_RID,
        "owned-service-profile-marker", ERROR_INVALID_SID);
    const std::wstring text = SidText(const_cast<BYTE*>(sid.data()));
    // Controller stdout is captured by CI. The SYSTEM worker has no captured
    // stdout; emit only this canonical, protected ownership marker's SID.
    std::printf("Protected launch: owned-profile-sid=%ls\n", text.c_str());
    std::fflush(stdout);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
      ++observed->attempts;
      const BOOL deleted = DeleteProfileW(text.c_str(), nullptr, nullptr);
      const DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
      observed->api_status = error;
      if (deleted || error == ERROR_FILE_NOT_FOUND || error == ERROR_NOT_FOUND) break;
      const ULONGLONG now = GetTickCount64();
      if ((error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) &&
          observed->attempts < 21 && now < deadline) {
        // All exact children and the owned holder have already stopped. Allow
        // a bounded OS release delay, without forcing hive unload or deletion.
        const ULONGLONG remaining = deadline - now;
        Sleep(static_cast<DWORD>(remaining < 250 ? remaining : 250));
        if (GetTickCount64() < deadline) continue;
      }
      HKEY hive = nullptr;
      observed->hive_status = static_cast<DWORD>(RegOpenKeyExW(HKEY_USERS, text.c_str(), 0, KEY_READ, &hive));
      if (hive) RegCloseKey(hive);
      return error ? error : ERROR_GEN_FAILURE;
    }
    return DeleteFileW(marker.c_str()) ? ERROR_SUCCESS : GetLastError();
  } catch (const Failure& failure) { return failure.status; }
}
void Denied(HANDLE desktop, const std::wstring& path, DWORD access) {
  Require(ImpersonateLoggedOnUser(desktop), "impersonate-desktop-negative");
  const HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  const DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (!RevertToSelf()) std::abort();
  Require(error == ERROR_ACCESS_DENIED, "desktop-mutation-denied", error ? error : ERROR_ACCESS_DENIED);
}
void DesktopHelper(HANDLE desktop, HANDLE job, PSID user, PSID capability) {
  const Local descriptor = Security();
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  std::wstring command = Quote(config.image) + L" --prepare-desktop " + Quote(SidText(user)) + L" " + Quote(SidText(capability));
  SIZE_T needed = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &needed);
  Require(needed != 0, "desktop-helper-attribute-size");
  std::vector<std::byte> memory(needed);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
  Require(InitializeProcThreadAttributeList(attributes, 1, 0, &needed), "desktop-helper-attributes");
  const auto release = [](LPPROC_THREAD_ATTRIBUTE_LIST value) { DeleteProcThreadAttributeList(value); };
  const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(release)> own(attributes, release);
  HANDLE jobs[] = {job};
  Require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs), nullptr, nullptr),
      "desktop-helper-atomic-job");
  wchar_t desktop_name[] = L"winsta0\\default";
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.lpDesktop = desktop_name;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  Require(CreateProcessAsUserW(desktop, config.image.c_str(), command.data(), &security, &security,
      FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, config.directory.c_str(),
      &startup.StartupInfo, &process),
      "desktop-helper-create");
  const Handle child(process.hProcess), thread(process.hThread);
  BOOL contained = FALSE;
  DWORD setup = ERROR_SUCCESS;
  if (!IsProcessInJob(child.get(), job, &contained)) setup = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
  else if (!contained) setup = ERROR_ACCESS_DENIED;
  else if (ResumeThread(thread.get()) == MAXDWORD) setup = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
  if (setup != ERROR_SUCCESS) {
    const DWORD error = setup;
    TerminateProcess(child.get(), error);
    WaitForSingleObject(child.get(), 5000);
    throw Failure{error, "desktop-helper-job"};
  }
  const DWORD waited = WaitForSingleObject(child.get(), 10000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(child.get(), ERROR_TIMEOUT);
    WaitForSingleObject(child.get(), 5000);
    throw Failure{ERROR_TIMEOUT, "desktop-helper-wait"};
  }
  DWORD code = 0;
  Require(GetExitCodeProcess(child.get(), &code), "desktop-helper-exit");
  Check(code, "desktop-helper-result");
}
void ObserveLaunchAccess(HANDLE token, Evidence* evidence) {
  // Observe the exact private identity supplied to Create. The launcher still
  // derives its restricted token and sealed default DACL independently.
  const auto error = [](bool success) {
    const DWORD status = success ? ERROR_SUCCESS : GetLastError();
    return success || status ? status : ERROR_GEN_FAILURE;
  };
  Require(ImpersonateLoggedOnUser(token), "impersonate-launch-preflight");
  const HANDLE image = CreateFileW(config.image.c_str(), GENERIC_READ | GENERIC_EXECUTE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  evidence->image_access = error(image != INVALID_HANDLE_VALUE);
  if (image != INVALID_HANDLE_VALUE) CloseHandle(image);
  const HANDLE directory = CreateFileW(config.directory.c_str(), GENERIC_READ | GENERIC_EXECUTE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  evidence->directory_access = error(directory != INVALID_HANDLE_VALUE);
  if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
  HANDLE duplicate = nullptr;
  evidence->target_token_access = error(DuplicateTokenEx(token,
      TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, nullptr,
      SecurityImpersonation, TokenPrimary, &duplicate) != FALSE);
  if (duplicate) CloseHandle(duplicate);
  HANDLE caller = nullptr;
  evidence->private_caller_token_access = error(OpenProcessToken(GetCurrentProcess(),
      TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &caller) != FALSE);
  if (caller) CloseHandle(caller);
  if (!RevertToSelf()) std::abort();
  caller = nullptr;
  evidence->system_caller_token_access = error(OpenProcessToken(GetCurrentProcess(),
      TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &caller) != FALSE);
  if (caller) CloseHandle(caller);
  // These observations distinguish access checks; the test never requires a
  // hypothesized cause of an earlier failure. All real launch assertions follow.
}
Trustee ClassifyTrustee(PSID sid, PSID private_user, PSID private_logon = nullptr) {
  if (!sid || !IsValidSid(sid)) return Trustee::unknown;
  if (EqualSid(sid, private_user)) return Trustee::private_user;
  if (private_logon && EqualSid(sid, private_logon)) return Trustee::private_logon;
  if (IsWellKnownSid(sid, WinLocalSystemSid)) return Trustee::system;
  if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return Trustee::administrators;
  if (IsWellKnownSid(sid, WinCreatorOwnerRightsSid)) return Trustee::owner_rights;
  if (IsWellKnownSid(sid, WinWorldSid)) return Trustee::everyone;
  if (IsWellKnownSid(sid, WinAuthenticatedUserSid)) return Trustee::authenticated;
  if (IsWellKnownSid(sid, WinLogonIdsSid)) return Trustee::logon;
  return Trustee::other;
}
TokenObjectEvidence ObserveTokenObject(HANDLE token, PSID private_user, PSID private_logon = nullptr) {
  TokenObjectEvidence result;
  PSID owner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  result.status = GetSecurityInfo(token, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &acl, nullptr, &descriptor);
  const Local release(descriptor);
  if (result.status != ERROR_SUCCESS) return result;
  result.owner = ClassifyTrustee(owner, private_user, private_logon);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  BOOL present = FALSE, defaulted = FALSE;
  if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      !GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted)) {
    result.status = GetLastError();
    return result;
  }
  result.control = control;
  result.present = static_cast<DWORD>(present != FALSE);
  result.defaulted = static_cast<DWORD>(defaulted != FALSE);
  result.null_acl = static_cast<DWORD>(acl == nullptr);
  if (!present || !acl) return result;
  if (!IsValidAcl(acl)) { result.status = ERROR_INVALID_ACL; return result; }
  result.count = acl->AceCount;
  for (DWORD i = 0; i < result.count && i < std::size(result.aces); ++i) {
    void* raw = nullptr;
    if (!GetAce(acl, i, &raw)) { result.status = GetLastError(); return result; }
    const auto* header = static_cast<const ACE_HEADER*>(raw);
    auto& observed = result.aces[result.recorded++];
    observed.type = header->AceType;
    observed.flags = header->AceFlags;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE) continue;
    // These two ACE forms share the mask/SID layout. Bound the SID before any
    // Win32 SID inspection; unfamiliar ACE forms remain explicitly unclassified.
    constexpr size_t offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
    if (header->AceSize < offset + 8) { result.status = ERROR_INVALID_ACL; return result; }
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    const size_t sid_bytes = 8 + static_cast<size_t>(*GetSidSubAuthorityCount(sid)) * sizeof(DWORD);
    if (sid_bytes > header->AceSize - offset || !IsValidSid(sid)) {
      result.status = ERROR_INVALID_SID;
      return result;
    }
    observed.mask = ace->Mask;
    observed.trustee = ClassifyTrustee(sid, private_user, private_logon);
  }
  if (result.recorded != result.count) result.status = ERROR_MORE_DATA;
  return result;
}
using CreateDiagnosticLowbox = BOOL(WINAPI*)(HANDLE, PSECURITY_CAPABILITIES, PHANDLE);
// Native signature is also exercised with zero extra handles by Chromium's
// base/win/access_token_unittest.cc. This fixture reports raw NTSTATUS separately
// from Win32; no diagnostic token can reach a process or a resource handoff.
using CreateDiagnosticNativeLowbox = LONG(NTAPI*)(PHANDLE, HANDLE, ACCESS_MASK, PVOID,
    PSID, DWORD, PSID_AND_ATTRIBUTES, DWORD, PHANDLE);
TokenCallEvidence ObserveDuplicate(HANDLE token, DWORD access) {
  TokenCallEvidence observed;
  HANDLE raw = nullptr;
  const BOOL ok = DuplicateTokenEx(token, access, nullptr, SecurityImpersonation, TokenPrimary, &raw);
  observed.status = ok ? ERROR_SUCCESS : GetLastError();
  observed.called = 1;
  observed.succeeded = static_cast<DWORD>(ok != FALSE);
  observed.returned_token = static_cast<DWORD>(raw && raw != INVALID_HANDLE_VALUE);
  const Handle close(raw);
  return observed;
}
TokenCallEvidence ObserveThreadReopen(DWORD access, BOOL as_self) {
  TokenCallEvidence observed;
  HANDLE raw = nullptr;
  const BOOL ok = OpenThreadToken(GetCurrentThread(), access, as_self, &raw);
  observed.status = ok ? ERROR_SUCCESS : GetLastError();
  observed.called = 1;
  observed.succeeded = static_cast<DWORD>(ok != FALSE);
  observed.returned_token = static_cast<DWORD>(raw && raw != INVALID_HANDLE_VALUE);
  const Handle close(raw);
  return observed;
}
TokenCallEvidence ObserveNativeLowbox(CreateDiagnosticNativeLowbox create, HANDLE token,
                                     const SECURITY_CAPABILITIES& capabilities, Handle* retained = nullptr) {
  TokenCallEvidence observed;
  if (!create) return observed;
  HANDLE raw = nullptr;
  const LONG status = create(&raw, token, TOKEN_ALL_ACCESS, nullptr, capabilities.AppContainerSid,
      capabilities.CapabilityCount, capabilities.Capabilities, 0, nullptr);
  observed.called = 1;
  observed.status = static_cast<DWORD>(status);
  observed.succeeded = static_cast<DWORD>(status >= 0);
  observed.returned_token = static_cast<DWORD>(raw && raw != INVALID_HANDLE_VALUE);
  Handle close(raw);
  if (retained && status >= 0 && observed.returned_token) *retained = std::move(close);
  return observed;
}
void ObserveTokenPhase(HANDLE token, PSID private_user, SECURITY_CAPABILITIES capabilities,
                       CreateDiagnosticLowbox wrapper, CreateDiagnosticNativeLowbox native,
                       TokenPhaseEvidence* observed, Handle* namespace_token = nullptr) {
  observed->object = ObserveTokenObject(token, private_user);
  observed->native_system = ObserveNativeLowbox(native, token, capabilities, namespace_token);
  if (!ImpersonateLoggedOnUser(token)) { observed->impersonate = GetLastError(); return; }
  // No throwing C++ operations occur while impersonating. The raw kernel call
  // precedes the private-only wrapper, which may initialize namespace objects.
  observed->impersonate = ERROR_SUCCESS;
  observed->thread_query_private = ObserveThreadReopen(TOKEN_QUERY, FALSE);
  observed->thread_query_self = ObserveThreadReopen(TOKEN_QUERY, TRUE);
  constexpr DWORD combined = TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE;
  observed->thread_combined_private = ObserveThreadReopen(combined, FALSE);
  observed->thread_combined_self = ObserveThreadReopen(combined, TRUE);
  observed->duplicate_launch = ObserveDuplicate(token, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY);
  observed->duplicate_all = ObserveDuplicate(token, TOKEN_ALL_ACCESS);
  observed->native_private = ObserveNativeLowbox(native, token, capabilities);
  if (wrapper) {
    HANDLE raw = nullptr;
    const BOOL ok = wrapper(token, &capabilities, &raw);
    observed->wrapper.status = ok ? ERROR_SUCCESS : GetLastError();
    observed->wrapper.called = 1;
    observed->wrapper.succeeded = static_cast<DWORD>(ok != FALSE);
    observed->wrapper.returned_token = static_cast<DWORD>(raw && raw != INVALID_HANDLE_VALUE);
    if (raw && raw != INVALID_HANDLE_VALUE) CloseHandle(raw);
  }
  if (!RevertToSelf()) std::abort();
}
bool SafeNamespacePath(std::wstring_view value) {
  if (value.empty() || value.size() >= kNamespacePathCharacters || value.find(L"\\\\") != std::wstring_view::npos) return false;
  for (wchar_t c : value) {
    if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
        c == L'\\' || c == L'-' || c == L'_')) return false;
  }
  return true;
}
void ObserveNamespacePath(HANDLE lowbox, NamespacePathEvidence* observed) {
  ULONG returned = 0;
  // Match the CLR's primary-token query form in dotnet/coreclr
  // release/2.0.0/src/vm/profattach.cpp: a validated AppContainer token, NULL SID.
  const BOOL ok = GetAppContainerNamedObjectPath(lowbox, nullptr,
      static_cast<ULONG>(std::size(observed->path)), observed->path, &returned);
  observed->status = ok ? ERROR_SUCCESS : GetLastError();
  observed->succeeded = static_cast<DWORD>(ok != FALSE);
  observed->returned_length = returned;
  const wchar_t* end = std::wmemchr(observed->path, L'\0', std::size(observed->path));
  if (ok && returned && returned <= std::size(observed->path) && end &&
      SafeNamespacePath({observed->path, static_cast<size_t>(end - observed->path)})) {
    observed->validation = ERROR_SUCCESS;
  } else observed->validation = ERROR_INVALID_DATA;
}
// Microsoft documents this read-only native entry point and its access masks:
// learn.microsoft.com/windows/win32/devnotes/ntopendirectoryobject.
using OpenDiagnosticDirectory = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
Handle ObserveDirectoryOpen(OpenDiagnosticDirectory open, const wchar_t* path, size_t length,
                            ACCESS_MASK access, DirectoryCallEvidence* observed) {
  if (!open || !path || !length || length >= kNamespacePathCharacters) return {};
  UNICODE_STRING name{};
  name.Buffer = const_cast<wchar_t*>(path);
  name.Length = static_cast<USHORT>(length * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.ObjectName = &name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
  HANDLE raw = nullptr;
  const LONG status = open(&raw, access, &attributes);
  observed->called = 1;
  observed->status = static_cast<DWORD>(status);
  observed->returned_handle = static_cast<DWORD>(raw && raw != INVALID_HANDLE_VALUE);
  Handle handle(raw);
  if (status < 0 || !observed->returned_handle) return {};
  return handle;
}
void AddNamespaceCandidate(std::wstring path, DWORD candidate, NamespaceEvidence* observed) {
  // One exact leaf and at most three ancestors, without enumeration. Shared
  // ancestors are deduplicated across the two explicitly labeled candidates.
  for (DWORD depth = 0; depth <= 3; ++depth) {
    Require(SafeNamespacePath(path) && path.front() == L'\\', "diagnostic-namespace-candidate", ERROR_INVALID_NAME);
    bool existing = false;
    for (DWORD i = 0; i < observed->count; ++i) {
      if (path == observed->directories[i].path) {
        observed->directories[i].candidates |= candidate;
        existing = true;
        break;
      }
    }
    if (!existing) {
      Require(observed->count < std::size(observed->directories), "diagnostic-namespace-count", ERROR_MORE_DATA);
      auto& added = observed->directories[observed->count++];
      added.candidates = candidate;
      added.depth = depth;
      std::wmemcpy(added.path, path.c_str(), path.size() + 1);
    }
    const size_t separator = path.find_last_of(L'\\');
    if (separator == 0 || separator == std::wstring::npos) break;
    path.resize(separator);
  }
}
bool BoundedSid(const void* buffer, DWORD bytes, PSID value) {
  const auto first = reinterpret_cast<std::uintptr_t>(buffer);
  const auto sid = reinterpret_cast<std::uintptr_t>(value);
  if (!value || sid < first || sid - first > bytes || bytes - (sid - first) < 8) return false;
  const size_t sid_bytes = 8 + static_cast<size_t>(*GetSidSubAuthorityCount(value)) * sizeof(DWORD);
  return sid_bytes <= bytes - (sid - first) && IsValidSid(value);
}
DWORD PrivateLogonSid(HANDLE token, void* output, DWORD capacity) {
  struct LogonBuffer { TOKEN_GROUPS groups; BYTE storage[SECURITY_MAX_SID_SIZE]; } buffer{};
  DWORD bytes = 0;
  if (!GetTokenInformation(token, TokenLogonSid, &buffer, sizeof(buffer), &bytes)) return GetLastError();
  if (bytes < sizeof(TOKEN_GROUPS) || bytes > sizeof(buffer) || buffer.groups.GroupCount != 1 ||
      (buffer.groups.Groups[0].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID ||
      !BoundedSid(&buffer, bytes, buffer.groups.Groups[0].Sid)) return ERROR_INVALID_DATA;
  PSID sid = buffer.groups.Groups[0].Sid;
  if (!CopySid(capacity, output, sid)) return GetLastError();
  return ERROR_SUCCESS;
}
Handle DirectoryAccessContext(HANDLE private_primary, const storage::AuthorityLaunchSpec& spec, DWORD* status) {
  try {
    HANDLE raw = nullptr;
    Require(DuplicateTokenEx(private_primary, TOKEN_QUERY, nullptr, SecurityImpersonation,
        TokenImpersonation, &raw), "directory-access-client-duplicate");
    Handle token(raw);
    const auto user = User(raw);
    TOKEN_STATISTICS expected{}, actual{};
    DWORD bytes = 0, session = 0;
    Require(GetTokenInformation(private_primary, TokenStatistics, &expected, sizeof(expected), &bytes) &&
        GetTokenInformation(raw, TokenStatistics, &actual, sizeof(actual), &bytes) &&
        GetTokenInformation(raw, TokenSessionId, &session, sizeof(session), &bytes), "directory-access-client-query");
    Require(actual.TokenType == TokenImpersonation && actual.ImpersonationLevel == SecurityImpersonation &&
        EqualSid(const_cast<BYTE*>(user.data()), spec.desktop_user) && session == spec.desktop_session &&
        actual.AuthenticationId.HighPart == expected.AuthenticationId.HighPart &&
        actual.AuthenticationId.LowPart == expected.AuthenticationId.LowPart &&
        actual.GroupCount == expected.GroupCount && actual.PrivilegeCount == expected.PrivilegeCount,
        "directory-access-client-identity", ERROR_ACCESS_DENIED);
    *status = ERROR_SUCCESS;
    return token;
  } catch (const Failure& failure) { *status = failure.status; }
  catch (...) { *status = ERROR_NOT_ENOUGH_MEMORY; }
  return {};
}
DirectoryAccessEvidence ObserveDirectoryCreateAccess(HANDLE directory, HANDLE private_client) {
  DirectoryAccessEvidence result;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL acl = nullptr;
  result.setup_status = GetSecurityInfo(directory, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      nullptr, nullptr, &acl, nullptr, &descriptor);
  const Local release(descriptor);
  if (result.setup_status != ERROR_SUCCESS) return result;
  if (!IsValidSecurityDescriptor(descriptor) || (acl && !IsValidAcl(acl))) {
    result.setup_status = ERROR_INVALID_SECURITY_DESCR;
    return result;
  }
  if (acl && acl->AceCount > 128) { result.setup_status = ERROR_MORE_DATA; return result; }
  for (DWORD i = 0; acl && i < acl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(acl, i, &raw)) { result.setup_status = GetLastError(); return result; }
    const auto* header = static_cast<const ACE_HEADER*>(raw);
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    if ((header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE) ||
        header->AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart)) {
      result.setup_status = ERROR_NOT_SUPPORTED;
      return result;
    }
    if (static_cast<const ACCESS_ALLOWED_ACE*>(raw)->Mask & (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL)) {
      // Only specific effective ACE masks are evaluated. Do not guess a native
      // object's generic mapping or turn an unsupported form into denial.
      result.setup_status = ERROR_GENERIC_NOT_MAPPED;
      return result;
    }
  }
  constexpr DWORD create_subdirectory = 0x0008;
  GENERIC_MAPPING mapping{};  // No requested or effective ACE mask is generic.
  struct Privileges { PRIVILEGE_SET header; LUID_AND_ATTRIBUTES extra[7]; } privileges{};
  DWORD privilege_bytes = sizeof(privileges), granted = 0;
  BOOL authorized = FALSE;
  const BOOL ok = AccessCheck(descriptor, private_client, create_subdirectory, &mapping,
      &privileges.header, &privilege_bytes, &granted, &authorized);
  result.called = 1;
  result.status = ok ? ERROR_SUCCESS : GetLastError();
  if (ok) {
    result.authorized = static_cast<DWORD>(authorized != FALSE);
    result.granted = granted;
  }
  return result;
}
void VerifyNamespaceToken(HANDLE lowbox, HANDLE private_primary, const storage::AuthorityLaunchSpec& spec) {
  Require(lowbox && lowbox != INVALID_HANDLE_VALUE, "diagnostic-namespace-lowbox-handle", ERROR_INVALID_HANDLE);
  const auto user = User(lowbox);
  TOKEN_STATISTICS expected{}, actual{};
  DWORD bytes = 0, session = 0, app_container = 0;
  Require(GetTokenInformation(private_primary, TokenStatistics, &expected, sizeof(expected), &bytes) &&
      GetTokenInformation(lowbox, TokenStatistics, &actual, sizeof(actual), &bytes) &&
      GetTokenInformation(lowbox, TokenSessionId, &session, sizeof(session), &bytes) &&
      GetTokenInformation(lowbox, TokenIsAppContainer, &app_container, sizeof(app_container), &bytes),
      "diagnostic-namespace-lowbox-identity-query");
  Require(actual.TokenType == TokenPrimary && app_container == 1 &&
      EqualSid(const_cast<BYTE*>(user.data()), spec.desktop_user) && session == spec.desktop_session &&
      actual.AuthenticationId.HighPart == expected.AuthenticationId.HighPart &&
      actual.AuthenticationId.LowPart == expected.AuthenticationId.LowPart,
      "diagnostic-namespace-lowbox-identity", ERROR_ACCESS_DENIED);
  struct PackageBuffer { TOKEN_APPCONTAINER_INFORMATION information; BYTE sid[SECURITY_MAX_SID_SIZE]; } package{};
  Require(GetTokenInformation(lowbox, TokenAppContainerSid, &package, sizeof(package), &bytes),
      "diagnostic-namespace-lowbox-package-query");
  Require(bytes >= sizeof(package.information) && bytes <= sizeof(package) &&
      BoundedSid(&package, bytes, package.information.TokenAppContainer),
      "diagnostic-namespace-lowbox-package-bounds", ERROR_INVALID_SID);
  PSID actual_package = package.information.TokenAppContainer;
  Require(EqualSid(actual_package, spec.capabilities.AppContainerSid), "diagnostic-namespace-lowbox-package", ERROR_INVALID_SID);
}
void ObserveNamespace(HANDLE lowbox, HANDLE private_primary, const storage::AuthorityLaunchSpec& spec, NamespaceEvidence* observed) {
  try {
    // Keep the API's queried object distinct from its caller context. The raw
    // token is never assigned to a process or impersonated; it exists only for
    // this identity-checked read-only query and is closed by the caller.
    VerifyNamespaceToken(lowbox, private_primary, spec);
    observed->token_validation = ERROR_SUCCESS;
    std::array<DWORD, SECURITY_MAX_SID_SIZE / sizeof(DWORD)> logon_sid{};
    observed->private_logon_status = PrivateLogonSid(private_primary, logon_sid.data(), sizeof(logon_sid));
    PSID private_logon = observed->private_logon_status == ERROR_SUCCESS ? logon_sid.data() : nullptr;
    // AccessCheck receives a query-only retained impersonation-token handle;
    // this duplicate is never set on a thread, assigned, or handed to a child.
    const Handle access_client = DirectoryAccessContext(private_primary, spec, &observed->access_context_status);
    DWORD bytes = 0;
    Require(ProcessIdToSessionId(GetCurrentProcessId(), &observed->worker_session), "diagnostic-worker-session");
    Require(GetTokenInformation(lowbox, TokenSessionId, &observed->token_session, sizeof(observed->token_session), &bytes),
        "diagnostic-token-session");
    ObserveNamespacePath(lowbox, &observed->system_path);
    if (ImpersonateLoggedOnUser(private_primary)) {
      observed->private_impersonate = ERROR_SUCCESS;
      ObserveNamespacePath(lowbox, &observed->private_path);
      if (!RevertToSelf()) std::abort();
    } else observed->private_impersonate = GetLastError();
    const bool private_valid = observed->private_path.validation == ERROR_SUCCESS;
    const auto& selected = private_valid ? observed->private_path : observed->system_path;
    observed->source_private = static_cast<DWORD>(private_valid);
    Require(selected.validation == ERROR_SUCCESS, "diagnostic-namespace-path", ERROR_INVALID_DATA);
    const std::wstring path(selected.path);
    const std::wstring suffix = L"\\" + SidText(spec.capabilities.AppContainerSid);
    Require(path.size() > suffix.size() && path.ends_with(suffix), "diagnostic-namespace-package", ERROR_INVALID_SID);
    if (path.front() == L'\\') AddNamespaceCandidate(path, 1, observed);
    else {
      // Historical Chromium CreateLowBoxObjectDirectory and the original
      // Cygwin AppContainer patch use this session-root shape. These are
      // candidate roots based on actual observed IDs, not an assertion about
      // which namespace the failed Windows wrapper used.
      AddNamespaceCandidate(L"\\Sessions\\" + std::to_wstring(observed->worker_session) + L"\\" + path, 2, observed);
      AddNamespaceCandidate(L"\\Sessions\\" + std::to_wstring(observed->token_session) + L"\\" + path, 4, observed);
    }
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto open = ntdll ? reinterpret_cast<OpenDiagnosticDirectory>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtOpenDirectoryObject"))) : nullptr;
    observed->directory_export = open ? ERROR_SUCCESS : (ntdll ? ERROR_PROC_NOT_FOUND : ERROR_MOD_NOT_FOUND);
    Check(observed->directory_export, "diagnostic-directory-export");
    constexpr ACCESS_MASK query_traverse = 0x0001 | 0x0002;
    for (DWORD i = 0; i < observed->count; ++i) {
      auto& directory = observed->directories[i];
      const size_t length = std::wcslen(directory.path);
      const Handle query = ObserveDirectoryOpen(open, directory.path, length, query_traverse, &directory.system_query);
      const Handle security = ObserveDirectoryOpen(open, directory.path, length, READ_CONTROL, &directory.system_security);
      if (security) {
        directory.security = ObserveTokenObject(security.get(), spec.desktop_user, private_logon);
        if (access_client) directory.private_create_subdirectory = ObserveDirectoryCreateAccess(security.get(), access_client.get());
      }
      if (!ImpersonateLoggedOnUser(private_primary)) { directory.impersonate = GetLastError(); continue; }
      directory.impersonate = ERROR_SUCCESS;
      {
        const Handle private_query = ObserveDirectoryOpen(open, directory.path, length, query_traverse, &directory.private_query);
        const Handle private_security = ObserveDirectoryOpen(open, directory.path, length, READ_CONTROL, &directory.private_security);
      }
      if (!RevertToSelf()) std::abort();
    }
    observed->status = ERROR_SUCCESS;
  } catch (const Failure& failure) {
    observed->status = failure.status;
    if (observed->token_validation == ERROR_NOT_READY) observed->token_validation = failure.status;
  }
  catch (...) { observed->status = ERROR_NOT_ENOUGH_MEMORY; }
}
void ObserveEquivalentToken(HANDLE token, const storage::AuthorityLaunchSpec& spec, EquivalentTokenEvidence* observed) {
  observed->attempted = 1;
  try {
    HANDLE current = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &current)) {
      CloseHandle(current);
      throw Failure{ERROR_ACCESS_DENIED, "diagnostic-requires-system-context"};
    }
    Require(GetLastError() == ERROR_NO_TOKEN, "diagnostic-thread-token-state");
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current), "diagnostic-caller-token");
    const Handle caller(current);
    const auto caller_sid = User(current);
    Require(IsWellKnownSid(const_cast<BYTE*>(caller_sid.data()), WinLocalSystemSid),
        "diagnostic-system-token", ERROR_ACCESS_DENIED);
    HANDLE raw = nullptr;
    Require(CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr,
        0, nullptr, &raw), "diagnostic-equivalent-derivation");
    const Handle equivalent(raw);
    const auto actual_user = User(raw);
    TOKEN_STATISTICS parent{}, derived{};
    DWORD session = 0, bytes = 0;
    Require(GetTokenInformation(token, TokenStatistics, &parent, sizeof(parent), &bytes) &&
        GetTokenInformation(raw, TokenStatistics, &derived, sizeof(derived), &bytes) &&
        GetTokenInformation(raw, TokenSessionId, &session, sizeof(session), &bytes), "diagnostic-equivalent-identity-query");
    Require(derived.TokenType == TokenPrimary && EqualSid(const_cast<BYTE*>(actual_user.data()), spec.desktop_user) &&
        session == spec.desktop_session && derived.AuthenticationId.HighPart == parent.AuthenticationId.HighPart &&
        derived.AuthenticationId.LowPart == parent.AuthenticationId.LowPart,
        "diagnostic-equivalent-identity", ERROR_ACCESS_DENIED);
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto wrapper = kernelbase ? reinterpret_cast<CreateDiagnosticLowbox>(
        reinterpret_cast<void*>(GetProcAddress(kernelbase, "CreateAppContainerToken"))) : nullptr;
    const auto native = ntdll ? reinterpret_cast<CreateDiagnosticNativeLowbox>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateLowBoxToken"))) : nullptr;
    observed->wrapper_export = wrapper ? ERROR_SUCCESS : (kernelbase ? ERROR_PROC_NOT_FOUND : ERROR_MOD_NOT_FOUND);
    observed->native_export = native ? ERROR_SUCCESS : (ntdll ? ERROR_PROC_NOT_FOUND : ERROR_MOD_NOT_FOUND);
    ObserveTokenPhase(raw, spec.desktop_user, spec.capabilities, wrapper, native, &observed->before);
    // Match the production default-DACL change; this does not modify the token
    // object's own security descriptor, which is queried in each phase above.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;WDWO;;;OW)(A;;GA;;;SY)",
        SDDL_REVISION_1, &descriptor, nullptr), "diagnostic-default-descriptor");
    const Local release(descriptor);
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    Require(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present && acl,
        "diagnostic-default-acl", ERROR_INVALID_ACL);
    TOKEN_DEFAULT_DACL defaults{acl};
    Require(SetTokenInformation(raw, TokenDefaultDacl, &defaults, sizeof(defaults)), "diagnostic-seal-default-dacl");
    Handle namespace_token;
    ObserveTokenPhase(raw, spec.desktop_user, spec.capabilities, wrapper, native, &observed->after, &namespace_token);
    ObserveNamespace(namespace_token.get(), raw, spec, &observed->object_namespace);
    observed->status = ERROR_SUCCESS;
    std::snprintf(observed->stage, sizeof(observed->stage), "%s", "complete");
  } catch (const Failure& failure) {
    observed->status = failure.status;
    std::snprintf(observed->stage, sizeof(observed->stage), "%s", failure.stage);
  } catch (...) {
    observed->status = ERROR_NOT_ENOUGH_MEMORY;
    std::snprintf(observed->stage, sizeof(observed->stage), "%s", "diagnostic-allocation");
  }
}
const char* TrusteeName(Trustee value) {
  switch (value) {
    case Trustee::private_user: return "private";
    case Trustee::system: return "system";
    case Trustee::administrators: return "administrators";
    case Trustee::owner_rights: return "owner-rights";
    case Trustee::everyone: return "everyone";
    case Trustee::authenticated: return "authenticated";
    case Trustee::logon: return "logon";
    case Trustee::private_logon: return "exact-private-logon";
    case Trustee::other: return "other";
    default: return "unclassified";
  }
}
void PrintTokenCall(const char* phase, const char* operation, const TokenCallEvidence& observed, bool native = false) {
  if (!observed.called) {
    std::printf("Protected launch: equivalent-token phase=%s operation=%s called=0\n", phase, operation);
  } else if (native) {
    std::printf("Protected launch: equivalent-token phase=%s operation=%s ntstatus=0x%08lx nt-success=%lu returned-token=%lu\n",
        phase, operation, static_cast<unsigned long>(observed.status), static_cast<unsigned long>(observed.succeeded),
        static_cast<unsigned long>(observed.returned_token));
  } else {
    std::printf("Protected launch: equivalent-token phase=%s operation=%s win32=%lu api-success=%lu returned-token=%lu\n",
        phase, operation, static_cast<unsigned long>(observed.status), static_cast<unsigned long>(observed.succeeded),
        static_cast<unsigned long>(observed.returned_token));
  }
}
void PrintTokenPhase(const char* phase, const TokenPhaseEvidence& observed) {
  const auto& object = observed.object;
  std::printf("Protected launch: equivalent-token phase=%s object-query-win32=%lu owner=%s control=0x%04lx dacl-present=%lu defaulted=%lu null=%lu aces=%lu recorded=%lu\n",
      phase, static_cast<unsigned long>(object.status), TrusteeName(object.owner), static_cast<unsigned long>(object.control),
      static_cast<unsigned long>(object.present), static_cast<unsigned long>(object.defaulted),
      static_cast<unsigned long>(object.null_acl), static_cast<unsigned long>(object.count), static_cast<unsigned long>(object.recorded));
  for (DWORD i = 0; i < object.recorded && i < std::size(object.aces); ++i) {
    const auto& ace = object.aces[i];
    std::printf("Protected launch: equivalent-token phase=%s ace=%lu type=%lu flags=0x%02lx mask=0x%08lx trustee=%s\n",
        phase, static_cast<unsigned long>(i), static_cast<unsigned long>(ace.type), static_cast<unsigned long>(ace.flags),
        static_cast<unsigned long>(ace.mask), TrusteeName(ace.trustee));
  }
  std::printf("Protected launch: equivalent-token phase=%s private-impersonation-win32=%lu\n",
      phase, static_cast<unsigned long>(observed.impersonate));
  PrintTokenCall(phase, "native-system-zero-handles", observed.native_system, true);
  PrintTokenCall(phase, "thread-query-open-as-private", observed.thread_query_private);
  PrintTokenCall(phase, "thread-query-open-as-self", observed.thread_query_self);
  PrintTokenCall(phase, "thread-combined-open-as-private", observed.thread_combined_private);
  PrintTokenCall(phase, "thread-combined-open-as-self", observed.thread_combined_self);
  PrintTokenCall(phase, "duplicate-private-launch-rights", observed.duplicate_launch);
  PrintTokenCall(phase, "duplicate-private-all-rights", observed.duplicate_all);
  PrintTokenCall(phase, "native-private-zero-handles", observed.native_private, true);
  PrintTokenCall(phase, "wrapper-private", observed.wrapper);
}
const wchar_t* NamespacePathForPrint(const wchar_t* path) {
  const wchar_t* end = std::wmemchr(path, L'\0', kNamespacePathCharacters);
  if (!end || !SafeNamespacePath({path, static_cast<size_t>(end - path)})) return L"<unavailable>";
  return path;
}
void PrintNamespacePath(const char* context, const NamespacePathEvidence& observed) {
  std::printf("Protected launch: namespace-api context=%s win32=%lu api-success=%lu returned-length=%lu validation-win32=%lu path=%ls\n",
      context, static_cast<unsigned long>(observed.status), static_cast<unsigned long>(observed.succeeded),
      static_cast<unsigned long>(observed.returned_length), static_cast<unsigned long>(observed.validation),
      NamespacePathForPrint(observed.path));
}
void PrintDirectoryCall(DWORD index, const char* operation, const DirectoryCallEvidence& observed) {
  if (!observed.called) {
    std::printf("Protected launch: namespace-directory=%lu operation=%s called=0\n", static_cast<unsigned long>(index), operation);
  } else {
    std::printf("Protected launch: namespace-directory=%lu operation=%s ntstatus=0x%08lx returned-handle=%lu\n",
        static_cast<unsigned long>(index), operation, static_cast<unsigned long>(observed.status),
        static_cast<unsigned long>(observed.returned_handle));
  }
}
void PrintNamespace(const NamespaceEvidence& observed) {
  std::printf("Protected launch: namespace-query-source=diagnostic-native-lowbox package-argument=null token-validation-win32=%lu caller-contexts=system,legacy-private-primary\n",
      static_cast<unsigned long>(observed.token_validation));
  std::printf("Protected launch: namespace-private-logon-query-win32=%lu accesscheck-client-validation-win32=%lu\n",
      static_cast<unsigned long>(observed.private_logon_status), static_cast<unsigned long>(observed.access_context_status));
  std::printf("Protected launch: namespace-observation win32=%lu worker-session=%lu token-session=%lu private-impersonation-win32=%lu directory-export-win32=%lu source-private=%lu count=%lu\n",
      static_cast<unsigned long>(observed.status), static_cast<unsigned long>(observed.worker_session),
      static_cast<unsigned long>(observed.token_session), static_cast<unsigned long>(observed.private_impersonate),
      static_cast<unsigned long>(observed.directory_export), static_cast<unsigned long>(observed.source_private),
      static_cast<unsigned long>(observed.count));
  PrintNamespacePath("system", observed.system_path);
  PrintNamespacePath("private", observed.private_path);
  bool valid = observed.count <= std::size(observed.directories);
  for (DWORD i = 0; valid && i < observed.count; ++i) {
    const auto& directory = observed.directories[i];
    const wchar_t* end = std::wmemchr(directory.path, L'\0', std::size(directory.path));
    valid = end && SafeNamespacePath({directory.path, static_cast<size_t>(end - directory.path)}) &&
        directory.path[0] == L'\\' && directory.candidates != 0 && (directory.candidates & ~7u) == 0 && directory.depth <= 3 &&
        directory.security.recorded <= std::size(directory.security.aces) && directory.security.recorded <= directory.security.count &&
        directory.private_create_subdirectory.called <= 1 && directory.private_create_subdirectory.authorized <= 1;
  }
  if (!valid) {
    // Malformed optional observations must not replace the earlier production
    // failure. Do not traverse or print unvalidated directory records.
    std::puts("Protected launch: namespace-observation invalid-bounded-evidence");
    return;
  }
  for (DWORD i = 0; i < observed.count; ++i) {
    const auto& directory = observed.directories[i];
    std::printf("Protected launch: namespace-directory=%lu absolute-api=%lu worker-session-candidate=%lu token-session-candidate=%lu depth=%lu private-impersonation-win32=%lu path=%ls\n",
        static_cast<unsigned long>(i), static_cast<unsigned long>((directory.candidates & 1) != 0),
        static_cast<unsigned long>((directory.candidates & 2) != 0), static_cast<unsigned long>((directory.candidates & 4) != 0),
        static_cast<unsigned long>(directory.depth),
        static_cast<unsigned long>(directory.impersonate), directory.path);
    PrintDirectoryCall(i, "system-query-traverse", directory.system_query);
    PrintDirectoryCall(i, "system-read-control", directory.system_security);
    PrintDirectoryCall(i, "private-query-traverse", directory.private_query);
    PrintDirectoryCall(i, "private-read-control", directory.private_security);
    const auto& access = directory.private_create_subdirectory;
    std::printf("Protected launch: namespace-directory=%lu private-create-subdirectory-accesscheck setup-win32=%lu called=%lu api-win32=%lu authorized=%lu granted=0x%08lx requested=0x00000008 scope=object-dacl-only\n",
        static_cast<unsigned long>(i), static_cast<unsigned long>(access.setup_status), static_cast<unsigned long>(access.called),
        static_cast<unsigned long>(access.status), static_cast<unsigned long>(access.authorized), static_cast<unsigned long>(access.granted));
    const auto& object = directory.security;
    std::printf("Protected launch: namespace-directory=%lu system-security-query-win32=%lu owner=%s control=0x%04lx dacl-present=%lu defaulted=%lu null=%lu aces=%lu recorded=%lu\n",
        static_cast<unsigned long>(i), static_cast<unsigned long>(object.status), TrusteeName(object.owner),
        static_cast<unsigned long>(object.control), static_cast<unsigned long>(object.present), static_cast<unsigned long>(object.defaulted),
        static_cast<unsigned long>(object.null_acl), static_cast<unsigned long>(object.count), static_cast<unsigned long>(object.recorded));
    for (DWORD j = 0; j < object.recorded; ++j) {
      const auto& ace = object.aces[j];
      std::printf("Protected launch: namespace-directory=%lu ace=%lu type=%lu flags=0x%02lx mask=0x%08lx trustee=%s\n",
          static_cast<unsigned long>(i), static_cast<unsigned long>(j), static_cast<unsigned long>(ace.type),
          static_cast<unsigned long>(ace.flags), static_cast<unsigned long>(ace.mask), TrusteeName(ace.trustee));
    }
  }
}
void PrintEquivalentToken(const EquivalentTokenEvidence& observed) {
  if (!observed.attempted) return;
  std::printf("Protected launch: post-failure-equivalent-token context=legacy-automatic-impersonation setup-stage=%s setup-win32=%lu wrapper-export-win32=%lu native-export-win32=%lu\n",
      observed.stage, static_cast<unsigned long>(observed.status), static_cast<unsigned long>(observed.wrapper_export),
      static_cast<unsigned long>(observed.native_export));
  // These sequential observations may reuse objects initialized by the failed
  // production wrapper or earlier observations; they are not isolated retries.
  PrintTokenPhase("before-default-seal", observed.before);
  PrintTokenPhase("after-default-seal", observed.after);
  PrintNamespace(observed.object_namespace);
}
Evidence RunSystem() {
  Evidence evidence;
  bool installed = false;
  bool production_attempted = false;
  storage::ProtectedIdentityCleanup cleanup;
  std::unique_ptr<storage::ProtectedIdentity> identity;
  std::unique_ptr<storage::AuthorityLaunch> child;
  container::Sid package, capability;
  std::wstring package_name;
  const char* stage = "system.begin";
  try {
    Check(static_cast<DWORD>(RegDisablePredefinedCacheEx()), "disable-predefined-registry-cache");
    AbsentRuntimeService();
    Check(storage::InstallRuntimeIdentityService(config.image), "install-owned-runtime-holder");
    installed = true;
    auto sid = RuntimeSid();
    AbsentRuntimeProfile(sid.data());
    Check(container::Grant(config.directory, sid.data(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true),
        "private-holder-execute-acl");
    Check(container::Grant(config.image, sid.data(), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false),
        "private-holder-image-acl");
    // This protected marker is written only after proving there is no earlier
    // profile. The controller can finish owned cleanup after a service timeout.
    WriteNew(config.directory + L"\\profile.sid", sid.data(), static_cast<DWORD>(sid.size()));
    const DWORD identity_status = storage::ProtectedIdentity::Create(config.image, config.session, &identity, &stage, &cleanup);
    Check(identity_status, stage);
    Require(EqualSid(identity->sid(), sid.data()), "actual-private-token-user", ERROR_ACCESS_DENIED);
    HANDLE raw = nullptr;
    Require(WTSQueryUserToken(config.session, &raw), "actual-wts-desktop-token");
    const Handle desktop(raw);
    BYTE admin_sid[SECURITY_MAX_SID_SIZE]{};
    DWORD admin_bytes = sizeof(admin_sid);
    Require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &admin_bytes), "negative-admin-sid");
    SID_AND_ATTRIBUTES disabled{admin_sid, 0};
    HANDLE filtered_raw = nullptr;
    Require(CreateRestrictedToken(desktop.get(), DISABLE_MAX_PRIVILEGE, 1, &disabled,
        0, nullptr, 0, nullptr, &filtered_raw), "filter-ordinary-desktop-token");
    const Handle filtered(filtered_raw);
    auto desktop_sid = User(desktop.get());
    PSID desktop_user = desktop_sid.data();
    Require(!EqualSid(desktop_user, identity->sid()), "distinct-desktop-and-runtime-user", ERROR_ACCESS_DENIED);
    for (DWORD access : {static_cast<DWORD>(FILE_WRITE_DATA), static_cast<DWORD>(WRITE_DAC), static_cast<DWORD>(DELETE)}) {
      Denied(filtered.get(), config.image, access);
    }
    package_name = L"Gesher.LaunchTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    Require(ImpersonateLoggedOnUser(identity->token()), "impersonate-private-profile");
    const DWORD profile = container::EnsureProfile(package_name, package_name, &package);
    if (!RevertToSelf()) std::abort();
    Check(profile, "create-owned-appcontainer");
    Check(container::CapabilitySid(container::kRuntimeFilesCapability, &capability), "runtime-file-capability");
    for (PSID reader : {identity->sid(), capability.value, desktop_user}) {
      Check(container::Grant(config.directory, reader, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true), "fixture-execute-acl");
      Check(container::Grant(config.image, reader, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false), "fixture-image-execute-acl");
    }
    DesktopHelper(desktop.get(), service_job.get(), identity->sid(), capability.value);
    const std::wstring temporary = config.directory + L"\\private-data";
    PrivateDirectory(temporary, true);
    Check(container::Grant(temporary, identity->sid(), FILE_ALL_ACCESS, true), "private-data-user-acl");
    Check(container::Grant(temporary, package.value, FILE_ALL_ACCESS, true), "private-data-lowbox-acl");
    Denied(filtered.get(), temporary, FILE_ADD_FILE);
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    Require(length && length < MAX_PATH, "windows-directory");
    std::vector<std::wstring> values{L"SystemRoot=" + std::wstring(windows), L"WINDIR=" + std::wstring(windows),
        L"PATH=" + config.directory + L";" + windows + L"\\System32"};
    Check(identity->AppendProfileEnvironment(&values), "actual-private-profile-environment");
    std::vector<std::wstring> duplicate{L"localappdata=C:\\untrusted"};
    Require(identity->AppendProfileEnvironment(&duplicate) == ERROR_INVALID_PARAMETER,
        "deny-profile-environment-override", ERROR_INVALID_DATA);
    wchar_t profile_buffer[MAX_PATH]{};
    DWORD profile_size = static_cast<DWORD>(std::size(profile_buffer));
    Require(GetUserProfileDirectoryW(identity->token(), profile_buffer, &profile_size), "independent-private-profile-path");
    const std::wstring profile_path(profile_buffer);
    const std::wstring package_sid = SidText(package.value);
    PWSTR container_path = nullptr;
    Require(ImpersonateLoggedOnUser(identity->token()), "impersonate-container-folder");
    const HRESULT folder_status = GetAppContainerFolderPath(package_sid.c_str(), &container_path);
    DWORD folder_error = folder_status == S_OK ? ERROR_SUCCESS : HRESULT_CODE(folder_status);
    DWORD folder_attributes = INVALID_FILE_ATTRIBUTES;
    if (folder_status == S_OK && container_path) {
      folder_attributes = GetFileAttributesW(container_path);
      if (folder_attributes == INVALID_FILE_ATTRIBUTES) folder_error = GetLastError();
    }
    if (!RevertToSelf()) std::abort();
    const auto free_folder = [](wchar_t* path) { CoTaskMemFree(path); };
    const std::unique_ptr<wchar_t, decltype(free_folder)> own_folder(container_path, free_folder);
    Require(folder_status == S_OK && container_path && folder_attributes != INVALID_FILE_ATTRIBUTES &&
        (folder_attributes & FILE_ATTRIBUTE_DIRECTORY), "private-appcontainer-folder", folder_error ? folder_error : ERROR_INVALID_DATA);
    const std::wstring_view folder(container_path);
    Require(folder.size() > profile_path.size() && folder[profile_path.size()] == L'\\' &&
        CompareStringOrdinal(folder.data(), static_cast<int>(profile_path.size()), profile_path.data(),
            static_cast<int>(profile_path.size()), TRUE) == CSTR_EQUAL,
        "appcontainer-folder-matches-private-profile", ERROR_ACCESS_DENIED);
    bool matched_profile = false;
    for (const auto& value : values) {
      if (value == L"USERPROFILE=" + profile_path) matched_profile = true;
    }
    Require(matched_profile, "private-environment-matches-token-profile", ERROR_INVALID_DATA);
    values.push_back(L"TEMP=" + temporary);
    values.push_back(L"TMP=" + temporary);
    std::wstring environment;
    for (const auto& value : values) { environment += value; environment.push_back(L'\0'); }
    environment.push_back(L'\0');
    SID_AND_ATTRIBUTES caps[] = {{capability.value, SE_GROUP_ENABLED}};
    const std::wstring arguments = L"--guest " + Quote(profile_path);
    const storage::AuthorityLaunchSpec spec{config.image.c_str(), config.directory.c_str(), arguments,
        environment, {package.value, caps, 1, 0}, identity->sid(), config.session, &cleanup.profile_error, service_job.get()};
    std::wstring shadowed_environment = environment;
    shadowed_environment.pop_back();
    shadowed_environment.append(L"systemroot=C:\\untrusted").append(2, L'\0');
    storage::AuthorityLaunchSpec shadowed = spec;
    shadowed.environment = shadowed_environment;
    Require(storage::AuthorityLaunch::Create(identity->token(), shadowed, &child) == ERROR_INVALID_PARAMETER && !child,
        "deny-case-insensitive-environment-shadow", ERROR_INVALID_DATA);
    ObserveLaunchAccess(identity->token(), &evidence);
    production_attempted = true;
    const DWORD created = storage::AuthorityLaunch::Create(identity->token(), spec, &child, &stage);
    if (created != ERROR_SUCCESS && std::strcmp(stage, "lowbox.create-token") == 0) {
      // The failed production object has already been destroyed. This is an
      // equivalently derived token, observed only after the real failure; no
      // diagnostic result retries launch or changes that primary result.
      ObserveEquivalentToken(identity->token(), spec, &evidence.equivalent);
    }
    Check(created, stage);
    Require(WaitForSingleObject(child->process(), 0) == WAIT_TIMEOUT, "exact-child-suspended");
    BOOL contained = FALSE;
    Require(IsProcessInJob(child->process(), service_job.get(), &contained) && contained, "inherited-lowbox-job");
    const DWORD mutations[] = {PROCESS_VM_WRITE, PROCESS_VM_OPERATION, PROCESS_DUP_HANDLE,
        PROCESS_TERMINATE, PROCESS_SUSPEND_RESUME, PROCESS_CREATE_THREAD, WRITE_DAC, WRITE_OWNER};
    for (DWORD access : mutations) {
      Require(ImpersonateLoggedOnUser(filtered.get()), "impersonate-process-negative");
      const HANDLE impostor = OpenProcess(access, FALSE, child->pid());
      const DWORD denied = impostor ? ERROR_SUCCESS : GetLastError();
      if (impostor) CloseHandle(impostor);
      if (!RevertToSelf()) std::abort();
      Require(denied == ERROR_ACCESS_DENIED, "desktop-child-mutation-denied", denied ? denied : ERROR_ACCESS_DENIED);
    }
    Check(child->Resume(), "resume-exact-lowbox");
    Require(WaitForSingleObject(child->process(), 10000) == WAIT_OBJECT_0, "guest-completion", ERROR_TIMEOUT);
    DWORD code = 0;
    Require(GetExitCodeProcess(child->process(), &code), "guest-exit-query");
    Check(code, "guest-private-environment-witness");
    Check(child->Stop(0), "idempotent-exited-stop");
    Require(child->Resume() == ERROR_PROCESS_ABORTED, "deny-resume-after-stop", ERROR_INVALID_DATA);
  } catch (const Failure& failure) {
    evidence.status = failure.status;
    std::snprintf(evidence.stage, sizeof(evidence.stage), "%s", failure.stage);
  } catch (...) { evidence.status = ERROR_NOT_ENOUGH_MEMORY; }
  const auto record_cleanup = [&evidence](DWORD error, const char* operation) {
    if (error != ERROR_SUCCESS && evidence.cleanup == ERROR_SUCCESS) {
      evidence.cleanup = error;
      std::snprintf(evidence.cleanup_stage, sizeof(evidence.cleanup_stage), "%s", operation);
    }
  };
  if (child) {
    const DWORD stopped = child->Stop(10000);
    record_cleanup(stopped, "stop-exact-child");
    child.reset();
  }
  record_cleanup(cleanup.profile_error.load(), "recorded-child-stop");
  if (package.value && identity && evidence.cleanup == ERROR_SUCCESS) {
    if (ImpersonateLoggedOnUser(identity->token())) {
      const DWORD deleted = container::DeleteContainerProfile(package_name);
      if (!RevertToSelf()) std::abort();
      record_cleanup(deleted, "delete-appcontainer");
    } else record_cleanup(GetLastError(), "impersonate-profile-cleanup");
  }
  if (identity) {
    const DWORD unloaded = identity->UnloadProfile(evidence.cleanup);
    record_cleanup(unloaded, "unload-private-profile");
    identity.reset();
  }
  if (installed) {
    const DWORD removed = storage::UninstallRuntimeIdentityService(config.image);
    record_cleanup(removed, "remove-runtime-holder");
  }
  // Leave the protected ownership marker for the controller. The user profile
  // is deleted only after this profile-calling process has itself exited.
  record_cleanup(cleanup.profile_error.load(), "recorded-profile-cleanup");
  record_cleanup(cleanup.privilege_restore_error, "restore-profile-privileges");
  if (config.scm_profile_control || (production_attempted && evidence.status != ERROR_SUCCESS)) {
    // This isolated comparison runs after the original worker cleanup. Its
    // separate result cannot change either the production or cleanup verdict.
    profile_control::ObserveScmProfileControl(config.image, config.directory,
        config.worker_deadline, &evidence.scm_control);
  }
  return evidence;
}
void Report(DWORD state, DWORD error = ERROR_SUCCESS) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  status.dwWin32ExitCode = error;
  SetServiceStatus(service_status, &status);
}
DWORD WINAPI Control(DWORD operation, DWORD, void*, void*) {
  if (operation == SERVICE_CONTROL_STOP) { SetEvent(service_stop.get()); return ERROR_SUCCESS; }
  return operation == SERVICE_CONTROL_INTERROGATE ? ERROR_SUCCESS : ERROR_CALL_NOT_IMPLEMENTED;
}
void WINAPI ServiceMain(DWORD, wchar_t**) {
  service_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  service_status = RegisterServiceCtrlHandlerExW(config.name.c_str(), Control, nullptr);
  if (!service_status) return;
  try {
    // Only this disposable test process grants the elevated controller a
    // retained termination handle for its independent timeout watchdog.
    const Local descriptor = Security();
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    Require(GetSecurityDescriptorDacl(descriptor.get(), &present, &acl, &defaulted) && present && acl,
        "system-service-controller-acl");
    Check(SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr), "system-service-controller-access");
    // The independently created controller job is exclusive and SY/BA-only.
    // Keep the Session0 service outside this desktop-session job. Every child
    // is assigned at creation through JOB_LIST; no job handle is inherited.
    service_job.reset(OpenJobObjectW(JOB_OBJECT_ALL_ACCESS, FALSE,
        (L"Global\\" + config.name).c_str()));
    Require(!!service_job, "open-owned-desktop-job");
  } catch (const Failure& failure) {
    Evidence evidence;
    evidence.status = failure.status;
    std::snprintf(evidence.stage, sizeof(evidence.stage), "%s", failure.stage);
    try { WriteNew(config.directory + L"\\result.bin", &evidence, sizeof(evidence)); } catch (...) {}
    Report(SERVICE_STOPPED, failure.status);
    return;
  }
  Report(SERVICE_RUNNING);
  Evidence evidence = RunSystem();
  try { WriteNew(config.directory + L"\\result.bin", &evidence, sizeof(evidence)); }
  catch (const Failure& failure) { evidence.status = failure.status; }
  Report(SERVICE_STOPPED, evidence.status ? evidence.status : evidence.cleanup);
}
DWORD Guest(const wchar_t* profile) {
  wchar_t value[8193]{};
  for (const wchar_t* name : {L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA"}) {
    const DWORD size = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (!size || size >= std::size(value)) return ERROR_INVALID_DATA;
    const size_t prefix = std::wcslen(profile);
    if (size < prefix || CompareStringOrdinal(value, static_cast<int>(prefix), profile,
        static_cast<int>(prefix), TRUE) != CSTR_EQUAL || (size > prefix && value[prefix] != L'\\')) return ERROR_ACCESS_DENIED;
  }
  const DWORD count = GetEnvironmentVariableW(L"TEMP", value, static_cast<DWORD>(std::size(value)));
  if (!count || count >= std::size(value)) return ERROR_INVALID_DATA;
  const std::wstring path = std::wstring(value) + L"\\guest.bin";
  const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) return GetLastError();
  DWORD written = 0;
  if (!WriteFile(file.get(), "GSH", 3, &written, nullptr)) {
    const DWORD error = GetLastError();
    return error ? error : ERROR_WRITE_FAULT;
  }
  return written == 3 ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
DWORD Controller(bool explicit_scm_control) {
  DWORD result = ERROR_SUCCESS;
  Service service;
  Handle process;
  Handle job;
  std::wstring directory, image;
  bool own_directory = false;
  profile_control::ScmProfileControlEvidence scm_control;
  try {
    BOOL admin = FALSE;
    BYTE admin_sid[SECURITY_MAX_SID_SIZE]{};
    DWORD sid_bytes = sizeof(admin_sid);
    Require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &sid_bytes) &&
        CheckTokenMembership(nullptr, admin_sid, &admin) && admin, "require-elevated-controller", ERROR_ELEVATION_REQUIRED);
    AbsentRuntimeService();
    DWORD session = 0;
    Require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0,
        "require-real-desktop-session", ERROR_NOT_READY);
    LPWSTR info = nullptr;
    DWORD size = 0;
    Require(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSConnectState, &info, &size), "query-wts-state");
    const bool active = size == sizeof(WTS_CONNECTSTATE_CLASS) && *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(info) == WTSActive;
    WTSFreeMemory(info);
    Require(active, "require-active-wts-session", ERROR_NOT_READY);
    storage::ProtectedIdentityCleanup denied_cleanup;
    std::unique_ptr<storage::ProtectedIdentity> denied_identity;
    const char* denied_stage = "none";
    const DWORD denied = storage::ProtectedIdentity::Create(Image(), session, &denied_identity,
        &denied_stage, &denied_cleanup);
    Require(denied == ERROR_ACCESS_DENIED && !denied_identity, "reject-desktop-authority-impostor", ERROR_ACCESS_DENIED);
    PWSTR program_files = nullptr;
    const HRESULT root = SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, 0, nullptr, &program_files);
    Require(root == S_OK && program_files, "program-files-root", HRESULT_CODE(root));
    directory = std::wstring(program_files) + L"\\Gesher protected-launch-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CoTaskMemFree(program_files);
    PrivateDirectory(directory);
    own_directory = true;
    image = directory + L"\\probe.exe";
    const auto binary = Read(Image(), 64 * 1024 * 1024);
    WriteNew(image, binary.data(), static_cast<DWORD>(binary.size()));
    const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    Require(!!manager, "controller-open-scm");
    const std::wstring name = L"GesherLaunchTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const Local descriptor = Security();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
    job.reset(CreateJobObjectW(&security, (L"Global\\" + name).c_str()));
    const DWORD job_error = GetLastError();
    const bool own_job = job && job_error != ERROR_ALREADY_EXISTS;
    if (!own_job) job.reset();
    Require(own_job, "exclusive-desktop-job",
        job_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : job_error);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
        "configure-desktop-job");
    // Compute the single watchdog deadline before service startup and pass the
    // same absolute value to the worker. No phase refreshes this ninety seconds.
    const ULONGLONG deadline = GetTickCount64() + 90000;
    std::wstring command = Quote(image) + L" --test-service " + name + L" " + std::to_wstring(session) +
        L" " + std::to_wstring(deadline);
    if (explicit_scm_control) command += L" --scm-profile-control";
    service.reset(CreateServiceW(manager.get(), name.c_str(), name.c_str(), SERVICE_START | SERVICE_STOP |
        SERVICE_QUERY_STATUS | DELETE, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        command.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr));
    Require(!!service, "create-owned-system-service");
    Require(StartServiceW(service.get(), 0, nullptr), "start-owned-system-service");
    for (;;) {
      SERVICE_STATUS_PROCESS state{};
      DWORD bytes = 0;
      Require(QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&state),
          sizeof(state), &bytes), "query-owned-system-service");
      if (!process && state.dwProcessId && state.dwCurrentState == SERVICE_RUNNING) {
        process.reset(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, state.dwProcessId));
        Require(!!process, "retain-exact-system-service");
        wchar_t actual[32768]{};
        DWORD capacity = static_cast<DWORD>(std::size(actual));
        Require(QueryFullProcessImageNameW(process.get(), 0, actual, &capacity) &&
            CompareStringOrdinal(actual, static_cast<int>(capacity), image.c_str(), static_cast<int>(image.size()), TRUE) == CSTR_EQUAL,
            "verify-system-service-image", ERROR_ACCESS_DENIED);
      }
      if (state.dwCurrentState == SERVICE_STOPPED) break;
      Require(GetTickCount64() < deadline, "bounded-system-service-run", ERROR_TIMEOUT);
      Sleep(25);
    }
    const auto data = Read(directory + L"\\result.bin", sizeof(Evidence));
    Require(data.size() == sizeof(Evidence), "complete-test-evidence", ERROR_INVALID_DATA);
    Evidence evidence{};
    std::memcpy(&evidence, data.data(), sizeof(evidence));
    Require(evidence.magic == 0x47504c54 && std::memchr(evidence.stage, 0, sizeof(evidence.stage)) &&
        std::memchr(evidence.cleanup_stage, 0, sizeof(evidence.cleanup_stage)) &&
        std::memchr(evidence.equivalent.stage, 0, sizeof(evidence.equivalent.stage)),
        "validate-test-evidence", ERROR_INVALID_DATA);
    std::printf("Protected launch: private-token-preflight-image=%lu directory=%lu target-token=%lu caller-as-private=%lu caller-as-system=%lu\n",
        static_cast<unsigned long>(evidence.image_access), static_cast<unsigned long>(evidence.directory_access),
        static_cast<unsigned long>(evidence.target_token_access), static_cast<unsigned long>(evidence.private_caller_token_access),
        static_cast<unsigned long>(evidence.system_caller_token_access));
    std::printf("Protected launch: stage=%s win32=%lu cleanup_win32=%lu cleanup_stage=%s\n", evidence.stage,
        static_cast<unsigned long>(evidence.status), static_cast<unsigned long>(evidence.cleanup), evidence.cleanup_stage);
    PrintEquivalentToken(evidence.equivalent);
    scm_control = evidence.scm_control;
    result = evidence.status ? evidence.status : evidence.cleanup;
  } catch (const Failure& failure) {
    std::fprintf(stderr, "Protected launch: stage=%s win32=%lu\n", failure.stage, static_cast<unsigned long>(failure.status));
    result = failure.status;
  } catch (...) { result = ERROR_NOT_ENOUGH_MEMORY; }
  DWORD cleanup_error = ERROR_SUCCESS;
  const char* cleanup_stage = "none";
  const auto record_cleanup = [&](DWORD error, const char* operation) {
    if (error != ERROR_SUCCESS && cleanup_error == ERROR_SUCCESS) {
      cleanup_error = error;
      cleanup_stage = operation;
    }
  };
  bool worker_stopped = !service;
  bool worker_exit_verified = false;
  if (service) {
    SERVICE_STATUS state{};
    ControlService(service.get(), SERVICE_CONTROL_STOP, &state);
    if (process && WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
      if (!TerminateProcess(process.get(), ERROR_TIMEOUT)) record_cleanup(GetLastError(), "terminate-system-worker");
      else if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) record_cleanup(ERROR_TIMEOUT, "wait-terminated-system-worker");
    }
    if (process) {
      worker_exit_verified = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
      worker_stopped = worker_exit_verified;
    }
    else {
      SERVICE_STATUS_PROCESS final{};
      DWORD bytes = 0;
      worker_stopped = QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
          reinterpret_cast<BYTE*>(&final), sizeof(final), &bytes) && final.dwCurrentState == SERVICE_STOPPED;
    }
    if (!worker_stopped) record_cleanup(ERROR_TIMEOUT, "wait-system-worker");
    if (!DeleteService(service.get())) record_cleanup(GetLastError(), "delete-system-service");
    service.reset();
    process.reset();
  }
  bool job_empty = !job;
  ULONGLONG control_cleanup_deadline = GetTickCount64();
  if (job) {
    if (!TerminateJobObject(job.get(), ERROR_OPERATION_ABORTED)) record_cleanup(GetLastError(), "terminate-desktop-job");
    const ULONGLONG deadline = GetTickCount64() + 10000;
    control_cleanup_deadline = deadline;
    do {
      JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
      if (!QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
        record_cleanup(GetLastError(), "query-desktop-job");
        break;
      }
      if (accounting.ActiveProcesses == 0) { job_empty = true; break; }
      Sleep(25);
    } while (GetTickCount64() < deadline);
    if (!job_empty) record_cleanup(ERROR_TIMEOUT, "wait-empty-desktop-job");
  }
  if (own_directory) {
    // Control artifacts are a separate sibling. Their cleanup cannot change
    // the original result or consume a new timeout. Wait for the exact worker
    // object before inspecting its manifest, including the timeout path.
    try {
      if (worker_exit_verified) {
        profile_control::CleanupScmProfileControl(image, directory,
            control_cleanup_deadline, &scm_control);
      } else {
        const DWORD attributes = GetFileAttributesW((directory + L"-scm-control").c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_PROCESS_ABORTED;
        if (attributes != INVALID_FILE_ATTRIBUTES || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
          scm_control.cleanup_attempted = 1;
          scm_control.cleanup_status = error;
          scm_control.artifacts_retained = 1;
          std::snprintf(scm_control.cleanup_stage, sizeof(scm_control.cleanup_stage), "%s", "unverified-primary-worker-exit");
        }
      }
    } catch (...) {
      scm_control.cleanup_attempted = 1;
      scm_control.cleanup_status = ERROR_NOT_ENOUGH_MEMORY;
      scm_control.artifacts_retained = 1;
      std::snprintf(scm_control.cleanup_stage, sizeof(scm_control.cleanup_stage), "%s", "controller-control-allocation");
    }
  }
  if (explicit_scm_control || scm_control.attempted || scm_control.artifacts_retained) {
    std::printf("Protected SCM control: trigger=%s\n", explicit_scm_control ?
        "explicit-opt-in" : "automatic-after-production-failure");
    profile_control::PrintScmProfileControlEvidence(scm_control);
  }
  bool profile_cleanup_verified = true;
  if (own_directory && !worker_exit_verified) {
    const DWORD marker = GetFileAttributesW((directory + L"\\profile.sid").c_str());
    const DWORD error = marker == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_PROCESS_ABORTED;
    if (marker != INVALID_FILE_ATTRIBUTES || error != ERROR_FILE_NOT_FOUND) {
      profile_cleanup_verified = false;
      record_cleanup(error, "verify-profile-worker-exit");
    }
  }
  if (own_directory && worker_stopped && job_empty && profile_cleanup_verified) {
    const DWORD removed = storage::UninstallRuntimeIdentityService(image);
    if (removed != ERROR_SUCCESS && removed != ERROR_SERVICE_DOES_NOT_EXIST) {
      record_cleanup(removed, "remove-runtime-holder");
    } else {
      // The holder is a separate SCM process outside the desktop job. Keep
      // the owned profile marker and files if its removal is unconfirmed.
      ProfileDeletionEvidence observed;
      const DWORD profile = DeleteOwnedProfile(directory, &observed);
      record_cleanup(profile, "delete-owned-profile");
      std::printf("Protected launch: controller-profile-delete-attempts=%lu api=%lu hive=%lu\n",
          static_cast<unsigned long>(observed.attempts), static_cast<unsigned long>(observed.api_status),
          static_cast<unsigned long>(observed.hive_status));
      DeleteFileW((directory + L"\\private-data\\guest.bin").c_str());
      RemoveDirectoryW((directory + L"\\private-data").c_str());
      DeleteFileW((directory + L"\\result.bin").c_str());
      if (!DeleteFileW(image.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) record_cleanup(error, "delete-fixture-image");
      }
      if (!RemoveDirectoryW(directory.c_str())) record_cleanup(GetLastError(), "delete-fixture-directory");
    }
  }
  if (cleanup_error != ERROR_SUCCESS) std::fprintf(stderr, "Protected launch: controller-cleanup=%lu cleanup-stage=%s\n",
      static_cast<unsigned long>(cleanup_error), cleanup_stage);
  if (result == ERROR_SUCCESS) result = cleanup_error;
  if (result == ERROR_SUCCESS) std::puts("PASS: actual private SCM identity, profile environment, lowbox child, desktop denial and bounded cleanup");
  return result;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  DWORD control_exit = ERROR_INVALID_PARAMETER;
  if (profile_control::DispatchScmProfileControlService(argc, argv, &control_exit)) return static_cast<int>(control_exit);
  if (argc == 2 && std::wstring_view(argv[1]) == L"runtime-identity") return storage::RunRuntimeIdentityService();
  if (argc == 4 && std::wstring_view(argv[1]) == L"--prepare-desktop") return static_cast<int>(storage::PrepareRuntimeDesktop(argv[2], argv[3]));
  if (argc == 3 && std::wstring_view(argv[1]) == L"--guest") return static_cast<int>(Guest(argv[2]));
  if ((argc == 5 || argc == 6) && std::wstring_view(argv[1]) == L"--test-service") {
    try {
      Require(argc == 5 || std::wstring_view(argv[5]) == L"--scm-profile-control",
          "test-control-argument", ERROR_INVALID_PARAMETER);
      config.image = Image();
      config.directory = config.image.substr(0, config.image.find_last_of(L'\\'));
      config.name = argv[2];
      wchar_t* end = nullptr;
      const unsigned long session = std::wcstoul(argv[3], &end, 10);
      Require(session && session < MAXDWORD && end && !*end, "test-session-argument", ERROR_INVALID_PARAMETER);
      config.session = static_cast<DWORD>(session);
      Require(DeadlineArgument(argv[4], &config.worker_deadline), "test-deadline-argument", ERROR_INVALID_PARAMETER);
      const ULONGLONG now = GetTickCount64();
      Require(config.worker_deadline > now && config.worker_deadline - now <= 90000,
          "test-existing-worker-deadline", ERROR_INVALID_PARAMETER);
      config.scm_profile_control = argc == 6;
      SERVICE_TABLE_ENTRYW entries[] = {{config.name.data(), ServiceMain}, {nullptr, nullptr}};
      return StartServiceCtrlDispatcherW(entries) ? 0 : static_cast<int>(GetLastError());
    } catch (...) { return 1; }
  }
  if (argc == 1) return static_cast<int>(Controller(false));
  if (argc == 2 && std::wstring_view(argv[1]) == L"--scm-profile-control") return static_cast<int>(Controller(true));
  return 2;
}
