// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Elevated, disposable Windows contract for the actual private identity and
// lowbox launcher. Internal service/guest modes exist only in this test image.
#include "../../compat/usb_storage/protected_identity.h"
#include "../../compat/usb_storage/authority_launch.h"
#include "../../compat/usb_storage/protected_storage_policy.h"
#include "../../compat/framework/app_container.h"
#include "usb_lowbox_iat_trace.h"
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
namespace lowbox_trace = gesher::usb_storage::test;
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
constexpr DWORD kSessionMutationMasks[] = {0x0004, 0x0008, DELETE, WRITE_DAC, WRITE_OWNER};
struct SessionMutationEvidence {
  DWORD before = 0, after = 0, real_open_status = ERROR_NOT_READY, real_opened = 0;
};
struct SessionSubjectEvidence {
  DWORD attempted = 0, checked = 0;
  SessionMutationEvidence mutations[std::size(kSessionMutationMasks)]{};
};
struct SessionObjectsEvidence {
  DWORD captured = 0, same_object = 0, granted_access = 0, exact_sd_delta = 0;
  DWORD private_open_called = 0, private_open_status = ERROR_NOT_READY;
  DWORD wrong_type_rejected = 0, wrong_session_rejected = 0;
  // Actual private, filtered desktop, suspended guest, alternate-package token.
  SessionSubjectEvidence subjects[4]{};
};
struct LeafOpenEvidence { DWORD called = 0, status = ERROR_NOT_READY, opened = 0; };
constexpr DWORD kChildEventUseAccess = EVENT_ALL_ACCESS & ~(WRITE_DAC | WRITE_OWNER);
constexpr DWORD kChildDirectoryAllAccess = 0x000f000f;
constexpr DWORD kChildDirectoryUseAccess = kChildDirectoryAllAccess & ~(WRITE_DAC | WRITE_OWNER);
struct ChildEventEvidence {
  DWORD called = 0, status = ERROR_NOT_READY, opened = 0;
  DWORD signaled = 0, signal_error = ERROR_SUCCESS;
  DWORD waited = WAIT_FAILED, wait_error = ERROR_SUCCESS;
  DWORD type_matches = 0, name_matches = 0, granted_access = 0;
};
struct PackagePipePinEvidence {
  DWORD called = 0, present = 0, info_ok = 0, info_status = ERROR_NOT_READY, flags = 0;
  DWORD granted_access = 0, type_matches = 0, device_name_matches = 0;
  wchar_t name[kNamespacePathCharacters]{};
};
struct PackageLeafEvidence {
  DWORD absent_before_launch = 0, captured = 0, control = 0, status = ERROR_NOT_READY;
  DWORD policy_verified = 0, launch_pin_verified = 0;
  DWORD object_created = 0, subdirectory_created = 0;
  ChildEventEvidence event_creation;
  DWORD expected_reuse = 0, null_pin_reuse = 0, independent_pin_lifetime = 0;
  DWORD wrong_type_rejected = 0, wrong_object_rejected = 0, changed_policy_rejected = 0;
  DWORD sacl_mutation_denials = 0, unprotected_reuse_rejected = 0, audit_reuse_rejected = 0;
  DWORD audit_preserved = 0, sacl_fixture_restored = 0;
  DWORD pipe_pin_verified = 0, pipe_policy_verified = 0, pipe_positive = 0;
  PackagePipePinEvidence pipe_pin;
  DWORD pipe_denials = 0, pipe_reused = 0, pipe_changed_policy_rejected = 0;
  DWORD policy_own_read = 0, policy_other_denied = 0, policy_write_denials = 0;
  DWORD policy_replacement = 0, policy_bad_path_denied = 0;
  wchar_t path[kNamespacePathCharacters]{};
  wchar_t security[2048]{}, event_security[1024]{}, directory_security[1024]{};
  // Actual suspended package, distinct package token, filtered desktop.
  LeafOpenEvidence opens[3][std::size(kSessionMutationMasks)]{};
  LeafOpenEvidence child_opens[2][3]{};
  DWORD child_denials[2][2]{};
  DWORD child_used[2]{};
};
struct NamespaceCollisionEvidence {
  DWORD attempted = 0, profile_created = 0, absent = 0, status = ERROR_NOT_READY;
  DWORD system_owner_control_created = 0, system_owner_attempted = 0, system_owner_status = ERROR_NOT_READY;
  DWORD private_owner_controls = 0, private_owner_status = ERROR_NOT_READY;
  DWORD private_group_count = 0, private_system_group_attributes = 0;
  wchar_t private_user[96]{}, private_token_owner[96]{};
  DWORD create_status = ERROR_NOT_READY, desktop_owner = 0, desktop_dacl_changed = 0;
  DWORD private_open_status = ERROR_NOT_READY;
  DWORD launch_called = 0, launch_status = ERROR_NOT_READY, returned_child = 0;
  DWORD stopped_child = ERROR_NOT_READY, same_object_after = 0;
  DWORD explicit_label = 0, policy_stage = 0;
  DWORD labeled_launch_status = ERROR_NOT_READY, labeled_policy_stage = 0;
  wchar_t path[kNamespacePathCharacters]{};
  // This disposable collision has a small fixed ACE set. Preserve the complete
  // SDDL with the existing fail-closed copy check, leaving room in the bounded
  // evidence payload for the actual pre/post-wrapper namespace readbacks.
  wchar_t security[2048]{};
  char stage[96] = "not-attempted";
};
struct Evidence {
  DWORD magic = 0x47504c54;
  DWORD status = ERROR_SUCCESS;
  DWORD cleanup = ERROR_SUCCESS;
  DWORD profile_identity_created = 0, profile_unload_attempted = 0;
  DWORD profile_unload_status = ERROR_NOT_READY, profile_privilege_restore_status = ERROR_NOT_READY;
  DWORD image_access = ERROR_NOT_READY;
  DWORD directory_access = ERROR_NOT_READY;
  DWORD target_token_access = ERROR_NOT_READY;
  DWORD private_caller_token_access = ERROR_NOT_READY;
  DWORD system_caller_token_access = ERROR_NOT_READY;
  storage::AuthorityNamespaceCreateResult namespace_create;
  DWORD namespace_process_privileges_unchanged = 0, namespace_no_thread_token = 0;
  DWORD namespace_context_status = ERROR_NOT_READY;
  SessionObjectsEvidence session_objects;
  SessionObjectsEvidence app_container_objects;
  PackageLeafEvidence package_leaf;
  NamespaceCollisionEvidence namespace_collision;
  lowbox_trace::LowboxIatTraceEvidence lowbox_trace;
  EquivalentTokenEvidence equivalent;
  profile_control::ScmProfileControlEvidence scm_control;
  char stage[96] = "complete";
  char cleanup_stage[64] = "none";
};
// Fixed diagnostic payload: one bounded exact-call trace, two API paths, eight
// directory observations, and at most twelve ACEs per object. The controller
// and trace printer validate counts/strings before using the received record.
static_assert(std::is_trivially_copyable_v<Evidence> &&
              std::is_standard_layout_v<Evidence> && sizeof(Evidence) <= 65536);
struct Configuration {
  std::wstring name, image, directory;
  DWORD session = 0;
  ULONGLONG worker_deadline = 0;
  bool scm_profile_control = false;
  bool lowbox_iat_trace = false;
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
struct FixtureOptions { bool scm_profile_control = false, lowbox_iat_trace = false; };
bool ParseFixtureOptions(int argc, wchar_t** argv, int first, FixtureOptions* options) {
  if (!options || first < 1 || first > argc || argc - first > 2) return false;
  for (int i = first; i < argc; ++i) {
    const std::wstring_view flag(argv[i]);
    if (flag == L"--scm-profile-control" && !options->scm_profile_control) options->scm_profile_control = true;
    else if (flag == L"--lowbox-iat-trace" && !options->lowbox_iat_trace) options->lowbox_iat_trace = true;
    else return false;
  }
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
std::vector<BYTE> CaptureAuthorityPrivilegeBytes(HANDLE token) {
  DWORD bytes = 0;
  Require(!GetTokenInformation(token, TokenPrivileges, nullptr, 0, &bytes) &&
      GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes >= sizeof(DWORD) && bytes <= 16384,
      "authority-privilege-snapshot-size", ERROR_INVALID_DATA);
  std::vector<BYTE> result(bytes);
  DWORD returned = 0;
  Require(GetTokenInformation(token, TokenPrivileges, result.data(), bytes, &returned) &&
      returned == bytes, "authority-privilege-snapshot");
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
class ScopedLowboxTrace final {
 public:
  ScopedLowboxTrace(bool enabled, DWORD target_session, Evidence* evidence) noexcept
      : evidence_(enabled ? evidence : nullptr) {
    if (evidence_) {
      evidence_->lowbox_trace.expected_target_session = target_session;
      // Setup and coverage gaps are diagnostic observations. The actual
      // production call still runs once; End also handles partial setup.
      lowbox_trace::BeginLowboxIatTrace(&evidence_->lowbox_trace);
    }
  }
  ~ScopedLowboxTrace() { End(); }
  ScopedLowboxTrace(const ScopedLowboxTrace&) = delete;
  ScopedLowboxTrace& operator=(const ScopedLowboxTrace&) = delete;
  DWORD End() noexcept {
    if (!evidence_) return restored_;
    Evidence* const evidence = evidence_;
    evidence_ = nullptr;
    restored_ = lowbox_trace::EndLowboxIatTrace(&evidence->lowbox_trace);
    if (restored_ != ERROR_SUCCESS && evidence->cleanup == ERROR_SUCCESS) {
      evidence->cleanup = restored_;
      std::snprintf(evidence->cleanup_stage, sizeof(evidence->cleanup_stage), "%s", "restore-lowbox-iat-observer");
    }
    return restored_;
  }
 private:
  Evidence* evidence_ = nullptr;
  DWORD restored_ = ERROR_SUCCESS;
};
struct SessionObjectsSnapshot {
  Handle object;
  Local before, after;
  std::wstring path;
};
OpenDiagnosticDirectory SessionDirectoryOpen() {
  const HMODULE module = GetModuleHandleW(L"ntdll.dll");
  const auto open = module ? reinterpret_cast<OpenDiagnosticDirectory>(
      reinterpret_cast<void*>(GetProcAddress(module, "NtOpenDirectoryObject"))) : nullptr;
  Require(open != nullptr, "session-objects-open-export", ERROR_PROC_NOT_FOUND);
  return open;
}
auto SessionObjectQuery() {
  const HMODULE module = GetModuleHandleW(L"ntdll.dll");
  const auto query = module ? reinterpret_cast<decltype(&NtQueryObject)>(
      reinterpret_cast<void*>(GetProcAddress(module, "NtQueryObject"))) : nullptr;
  Require(query != nullptr, "session-objects-query-export", ERROR_PROC_NOT_FOUND);
  return query;
}
bool SameSessionObject(HANDLE first, HANDLE second) {
  const HMODULE module = GetModuleHandleW(L"kernelbase.dll");
  const auto compare = module ? reinterpret_cast<decltype(&CompareObjectHandles)>(
      reinterpret_cast<void*>(GetProcAddress(module, "CompareObjectHandles"))) : nullptr;
  Require(compare != nullptr, "session-objects-compare-export", ERROR_PROC_NOT_FOUND);
  return compare(first, second) != FALSE;
}
std::wstring SessionObjectText(HANDLE object, OBJECT_INFORMATION_CLASS information) {
  alignas(PUBLIC_OBJECT_TYPE_INFORMATION) BYTE buffer[4096]{};
  ULONG returned = 0;
  Require(SessionObjectQuery()(object, information, buffer, sizeof(buffer), &returned) >= 0 &&
      returned >= sizeof(UNICODE_STRING) && returned <= sizeof(buffer),
      "session-objects-query-text", ERROR_INVALID_DATA);
  const auto& text = *reinterpret_cast<const UNICODE_STRING*>(buffer);
  const auto base = reinterpret_cast<ULONG_PTR>(buffer);
  const auto start = reinterpret_cast<ULONG_PTR>(text.Buffer);
  Require(text.Buffer && text.Length && !(text.Length % sizeof(wchar_t)) &&
      text.Length <= text.MaximumLength && start >= base && start - base <= returned &&
      text.Length <= returned - (start - base), "session-objects-query-text-bounds", ERROR_INVALID_DATA);
  return {text.Buffer, text.Length / sizeof(wchar_t)};
}
DWORD SessionObjectAccess(HANDLE object) {
  PUBLIC_OBJECT_BASIC_INFORMATION information{};
  ULONG returned = 0;
  Require(SessionObjectQuery()(object, ObjectBasicInformation, &information, sizeof(information), &returned) >= 0,
      "session-objects-query-access", ERROR_INVALID_HANDLE);
  return information.GrantedAccess;
}
Handle OpenSessionDirectory(const std::wstring& path, DWORD access, HANDLE subject, DWORD* observed) {
  Require(!path.empty() && path.size() < kNamespacePathCharacters, "session-objects-path-bound", ERROR_INVALID_NAME);
  const auto open = SessionDirectoryOpen();
  UNICODE_STRING name{};
  name.Buffer = const_cast<wchar_t*>(path.data());
  name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.ObjectName = &name;
  // OBJ_DONT_REPARSE: this positive control must be the named directory itself.
  attributes.Attributes = OBJ_CASE_INSENSITIVE | 0x00001000;
  if (subject) Require(ImpersonateLoggedOnUser(subject), "session-objects-impersonate-subject");
  HANDLE raw = nullptr;
  const LONG status = open(&raw, access, &attributes);
  if (subject && !RevertToSelf()) std::abort();
  Handle result(raw);
  *observed = static_cast<DWORD>(status);
  Require((status >= 0) == static_cast<bool>(result), "session-objects-open-result", ERROR_INVALID_DATA);
  return Handle(result.release());
}
Local SessionObjectSecurity(HANDLE object) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(GetSecurityInfo(object, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
      nullptr, nullptr, nullptr, nullptr, &descriptor), "session-objects-security-snapshot");
  Local owned(descriptor);
  Require(descriptor && IsValidSecurityDescriptor(descriptor) && GetSecurityDescriptorLength(descriptor) <= 65536,
      "session-objects-security-bound", ERROR_INVALID_SECURITY_DESCR);
  return owned;
}
Local FullSaclSecurity(HANDLE object) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(GetSecurityInfo(object, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
      SACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
      nullptr, nullptr, nullptr, nullptr, &descriptor), "fixture-full-sacl-snapshot");
  Local owned(descriptor);
  Require(descriptor && IsValidSecurityDescriptor(descriptor) && GetSecurityDescriptorLength(descriptor) <= 16384,
      "fixture-full-sacl-bound", ERROR_INVALID_SECURITY_DESCR);
  return owned;
}
Handle OpenFixtureSacl(const std::wstring& path) {
  HANDLE raw = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw),
      "fixture-sacl-system-token");
  const Handle process(raw);
  const auto process_before = CaptureAuthorityPrivilegeBytes(process.get());
  raw = nullptr;
  Require(DuplicateTokenEx(process.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE,
      nullptr, SecurityImpersonation, TokenImpersonation, &raw), "fixture-sacl-private-token");
  const Handle token(raw);
  const auto before = CaptureAuthorityPrivilegeBytes(token.get());
  TOKEN_PRIVILEGES requested{}, previous{};
  requested.PrivilegeCount = 1;
  Require(LookupPrivilegeValueW(nullptr, L"SeSecurityPrivilege", &requested.Privileges[0].Luid),
      "fixture-sacl-privilege-name");
  requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  DWORD bytes = 0;
  SetLastError(ERROR_SUCCESS);
  Require(AdjustTokenPrivileges(token.get(), FALSE, &requested, sizeof(previous), &previous, &bytes) &&
      GetLastError() == ERROR_SUCCESS && bytes <= sizeof(previous) && previous.PrivilegeCount <= 1,
      "fixture-sacl-enable-private-privilege");
  DWORD opened = 0;
  Handle object = OpenSessionDirectory(path, READ_CONTROL | ACCESS_SYSTEM_SECURITY, token.get(), &opened);
  SetLastError(ERROR_SUCCESS);
  Require(AdjustTokenPrivileges(token.get(), FALSE, &previous, 0, nullptr, nullptr) &&
      GetLastError() == ERROR_SUCCESS && CaptureAuthorityPrivilegeBytes(token.get()) == before &&
      CaptureAuthorityPrivilegeBytes(process.get()) == process_before,
      "fixture-sacl-restore-private-privilege", ERROR_ACCESS_DENIED);
  Require(opened == 0 && object && SessionObjectAccess(object.get()) == (READ_CONTROL | ACCESS_SYSTEM_SECURITY),
      "fixture-sacl-exact-temporary-handle", ERROR_ACCESS_DENIED);
  return object;
}
using SetDiagnosticSecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
SetDiagnosticSecurity SecuritySetter() {
  const auto setter = reinterpret_cast<SetDiagnosticSecurity>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetSecurityObject")));
  Require(setter != nullptr, "fixture-security-set-export", ERROR_PROC_NOT_FOUND);
  return setter;
}
struct FixtureSaclRestore {
  SetDiagnosticSecurity set;
  HANDLE object;
  PSECURITY_DESCRIPTOR original;
  bool dirty = false;
  NTSTATUS Restore() {
    if (!dirty) return 0;
    // Leave the captured original immutable. SC+SI requests restoration of SI;
    // Windows consumes SC, and the fixture compares the resulting exact control.
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    PACL sacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    SECURITY_DESCRIPTOR descriptor{};
    constexpr WORD control_bits = SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED | SE_SACL_AUTO_INHERIT_REQ;
    if (!GetSecurityDescriptorControl(original, &control, &revision) || revision != SECURITY_DESCRIPTOR_REVISION ||
        !GetSecurityDescriptorSacl(original, &present, &sacl, &defaulted) || !present || !sacl ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorSacl(&descriptor, TRUE, sacl, defaulted)) return static_cast<NTSTATUS>(0xc0000079);
    WORD requested_control = static_cast<WORD>(control & control_bits);
    if (control & SE_SACL_AUTO_INHERITED) requested_control |= SE_SACL_AUTO_INHERIT_REQ;
    if (!SetSecurityDescriptorControl(&descriptor, control_bits, requested_control)) return static_cast<NTSTATUS>(0xc0000079);
    const NTSTATUS result = set(object, SACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION, &descriptor);
    if (result >= 0) dirty = false;
    return result;
  }
  ~FixtureSaclRestore() { if (dirty && Restore() < 0) std::abort(); }
};
SessionObjectsSnapshot CaptureSessionObjects(DWORD session, const wchar_t* root, SessionObjectsEvidence* evidence) {
  SessionObjectsSnapshot snapshot;
  snapshot.path = L"\\Sessions\\" + std::to_wstring(session) + L"\\" + root;
  DWORD status = 0;
  snapshot.object = OpenSessionDirectory(snapshot.path, 0x00020003, nullptr, &status);
  Require(status == 0 && snapshot.object, "session-objects-capture-open", ERROR_ACCESS_DENIED);
  Require(SessionObjectText(snapshot.object.get(), ObjectTypeInformation) == L"Directory" &&
      SessionObjectText(snapshot.object.get(), static_cast<OBJECT_INFORMATION_CLASS>(1)) == snapshot.path,
      "session-objects-capture-identity", ERROR_NOT_SAME_OBJECT);
  snapshot.before = SessionObjectSecurity(snapshot.object.get());
  evidence->captured = 1;
  return snapshot;
}
std::vector<std::vector<BYTE>> SessionAclBytes(PACL acl) {
  std::vector<std::vector<BYTE>> result;
  if (!acl) return result;
  Require(IsValidAcl(acl) && acl->AceCount <= 128, "session-objects-acl-bound", ERROR_INVALID_ACL);
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* raw = nullptr;
    Require(GetAce(acl, i, &raw), "session-objects-get-ace");
    const auto* header = static_cast<const ACE_HEADER*>(raw);
    Require(header->AceSize >= sizeof(ACE_HEADER), "session-objects-ace-bound", ERROR_INVALID_ACL);
    const auto* bytes = static_cast<const BYTE*>(raw);
    result.emplace_back(bytes, bytes + header->AceSize);
  }
  return result;
}
bool SessionGrant(const std::vector<BYTE>& bytes, PSID subject, DWORD mask) {
  if (bytes.size() < offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8) return false;
  const auto* ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(bytes.data());
  PSID sid = const_cast<DWORD*>(&ace->SidStart);
  return ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
      ace->Mask == mask && BoundedSid(bytes.data(), static_cast<DWORD>(bytes.size()), sid) &&
      bytes.size() == offsetof(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(sid) && EqualSid(sid, subject);
}
void VerifySessionObjectDelta(SessionObjectsSnapshot* snapshot, PSID subject, DWORD mask, SessionObjectsEvidence* evidence) {
  snapshot->after = SessionObjectSecurity(snapshot->object.get());
  SECURITY_DESCRIPTOR_CONTROL before_control{}, after_control{};
  DWORD before_revision = 0, after_revision = 0;
  Require(GetSecurityDescriptorControl(snapshot->before.get(), &before_control, &before_revision) &&
      GetSecurityDescriptorControl(snapshot->after.get(), &after_control, &after_revision) &&
      before_control == after_control && before_revision == after_revision,
      "session-objects-control-preserved", ERROR_INVALID_SECURITY_DESCR);
  for (const bool owner : {true, false}) {
    PSID before = nullptr, after = nullptr;
    BOOL before_default = FALSE, after_default = FALSE;
    const BOOL first = owner ? GetSecurityDescriptorOwner(snapshot->before.get(), &before, &before_default) :
        GetSecurityDescriptorGroup(snapshot->before.get(), &before, &before_default);
    const BOOL second = owner ? GetSecurityDescriptorOwner(snapshot->after.get(), &after, &after_default) :
        GetSecurityDescriptorGroup(snapshot->after.get(), &after, &after_default);
    Require(first && second && before && after && IsValidSid(before) && IsValidSid(after) &&
        EqualSid(before, after) && before_default == after_default,
        "session-objects-owner-group-preserved", ERROR_INVALID_SECURITY_DESCR);
  }
  PACL before_label = nullptr, after_label = nullptr, before_acl = nullptr, after_acl = nullptr;
  BOOL before_present = FALSE, after_present = FALSE, before_default = FALSE, after_default = FALSE;
  Require(GetSecurityDescriptorSacl(snapshot->before.get(), &before_present, &before_label, &before_default) &&
      GetSecurityDescriptorSacl(snapshot->after.get(), &after_present, &after_label, &after_default) &&
      before_present == after_present && before_default == after_default &&
      (before_label == nullptr) == (after_label == nullptr) &&
      (!before_label || before_label->AclRevision == after_label->AclRevision) &&
      SessionAclBytes(before_label) == SessionAclBytes(after_label),
      "session-objects-label-preserved", ERROR_INVALID_SECURITY_DESCR);
  Require(GetSecurityDescriptorDacl(snapshot->before.get(), &before_present, &before_acl, &before_default) &&
      GetSecurityDescriptorDacl(snapshot->after.get(), &after_present, &after_acl, &after_default) &&
      before_present && after_present && before_acl && after_acl && before_default == after_default &&
      before_acl->AclRevision == after_acl->AclRevision,
      "session-objects-dacl-preserved", ERROR_INVALID_ACL);
  const auto before = SessionAclBytes(before_acl);
  const auto after = SessionAclBytes(after_acl);
  size_t original = 0;
  DWORD before_grants = 0, after_grants = 0;
  for (const auto& ace : before) if (SessionGrant(ace, subject, mask)) ++before_grants;
  for (const auto& ace : after) {
    if (SessionGrant(ace, subject, mask)) {
      ++after_grants;
      if (!before_grants) continue;
    }
    Require(original < before.size() && ace == before[original++],
        "session-objects-unrelated-aces-preserved", ERROR_INVALID_ACL);
  }
  Require(original == before.size() && before_grants <= 1 && after_grants == 1 &&
      after.size() == before.size() + (before_grants ? 0 : 1),
      "session-objects-only-exact-private-grant", ERROR_INVALID_ACL);
  evidence->exact_sd_delta = 1;
}
DWORD SessionAllowed(PSECURITY_DESCRIPTOR descriptor, HANDLE client, DWORD mask) {
  PACL acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  Require(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present && acl,
      "session-objects-access-dacl", ERROR_INVALID_ACL);
  for (const auto& bytes : SessionAclBytes(acl)) {
    const auto* header = reinterpret_cast<const ACE_HEADER*>(bytes.data());
    if (header->AceFlags & INHERIT_ONLY_ACE) continue;
    Require((header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE) &&
        bytes.size() >= offsetof(ACCESS_ALLOWED_ACE, SidStart), "session-objects-access-ace-kind", ERROR_NOT_SUPPORTED);
    Require(!(reinterpret_cast<const ACCESS_ALLOWED_ACE*>(bytes.data())->Mask &
        (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL)),
        "session-objects-specific-access-masks", ERROR_GENERIC_NOT_MAPPED);
  }
  GENERIC_MAPPING mapping{};  // Requested and effective ACE masks are specific.
  struct Privileges { PRIVILEGE_SET set; LUID_AND_ATTRIBUTES extra[7]; } privileges{};
  DWORD size = sizeof(privileges), granted = 0;
  BOOL authorized = FALSE;
  Require(AccessCheck(descriptor, client, mask, &mapping, &privileges.set, &size, &granted, &authorized),
      "session-objects-accesscheck");
  Require(!authorized || granted == mask, "session-objects-accesscheck-mask", ERROR_INVALID_DATA);
  return authorized ? granted : 0;
}
void VerifySessionSubject(SessionObjectsSnapshot& snapshot, HANDLE subject, DWORD session,
    SessionSubjectEvidence* evidence, DWORD permitted_additions = 0) {
  evidence->attempted = 1;
  const auto user = User(subject);
  storage::AuthorityLaunchSpec identity{};
  identity.desktop_user = const_cast<BYTE*>(user.data());
  identity.desktop_session = session;
  DWORD error = ERROR_NOT_READY;
  const Handle client = DirectoryAccessContext(subject, identity, &error);
  Check(error, "session-objects-exact-access-client");
  Require(static_cast<bool>(client), "session-objects-access-client-handle", ERROR_INVALID_HANDLE);
  DWORD source_lowbox = 0, client_lowbox = 0, returned = 0;
  Require(GetTokenInformation(subject, TokenIsAppContainer, &source_lowbox, sizeof(source_lowbox), &returned) &&
      GetTokenInformation(client.get(), TokenIsAppContainer, &client_lowbox, sizeof(client_lowbox), &returned) &&
      source_lowbox == client_lowbox, "session-objects-client-lowbox-preserved", ERROR_INVALID_DATA);
  for (size_t i = 0; i < std::size(kSessionMutationMasks); ++i) {
    auto& observed = evidence->mutations[i];
    const DWORD mask = kSessionMutationMasks[i];
    observed.before = SessionAllowed(snapshot.before.get(), client.get(), mask);
    observed.after = SessionAllowed(snapshot.after.get(), client.get(), mask);
    Require((observed.after & ~(observed.before | permitted_additions)) == 0,
        "session-objects-no-unapproved-mutation-right", ERROR_ACCESS_DENIED);
    const Handle opened = OpenSessionDirectory(snapshot.path, mask, subject, &observed.real_open_status);
    observed.real_opened = static_cast<DWORD>(static_cast<bool>(opened));
    if (opened) {
      Require(((observed.before | permitted_additions) & mask) == mask && observed.after == mask &&
          SameSessionObject(snapshot.object.get(), opened.get()) && SessionObjectAccess(opened.get()) == mask,
          "session-objects-live-open-not-new", ERROR_ACCESS_DENIED);
    } else {
      Require(observed.real_open_status == 0xc0000022, "session-objects-live-denial-status", ERROR_INVALID_DATA);
    }
    ++evidence->checked;
  }
}
void VerifySessionLease(SessionObjectsSnapshot* snapshot, storage::ProtectedIdentity& identity,
    HANDLE borrowed, DWORD private_grant, SessionObjectsEvidence* evidence) {
  DWORD flags = 0;
  Require(borrowed && GetHandleInformation(borrowed, &flags) && !(flags & HANDLE_FLAG_INHERIT) &&
      SessionObjectText(borrowed, ObjectTypeInformation) == L"Directory" &&
      SessionObjectText(borrowed, static_cast<OBJECT_INFORMATION_CLASS>(1)) == snapshot->path &&
      SameSessionObject(snapshot->object.get(), borrowed), "session-objects-borrowed-exact-object", ERROR_NOT_SAME_OBJECT);
  evidence->same_object = 1;
  evidence->granted_access = SessionObjectAccess(borrowed);
  Require(evidence->granted_access == 0x00020003, "session-objects-borrowed-exact-rights", ERROR_ACCESS_DENIED);
  VerifySessionObjectDelta(snapshot, identity.sid(), private_grant, evidence);
  evidence->private_open_called = 1;
  const Handle opened = OpenSessionDirectory(snapshot->path, private_grant, identity.token(), &evidence->private_open_status);
  Require(evidence->private_open_status == 0 && opened && SameSessionObject(snapshot->object.get(), opened.get()) &&
      SessionObjectAccess(opened.get()) == private_grant,
      "session-objects-private-real-grant", ERROR_ACCESS_DENIED);
  VerifySessionSubject(*snapshot, identity.token(), identity.session(), &evidence->subjects[0], private_grant & 0x000c);
}
std::wstring PackageObjectPath(HANDLE identity, PSID package, DWORD session) {
  wchar_t path[kNamespacePathCharacters]{};
  ULONG returned = 0;
  Require(ImpersonateLoggedOnUser(identity), "package-path-impersonate");
  const BOOL queried = GetAppContainerNamedObjectPath(nullptr, package,
      static_cast<ULONG>(std::size(path)), path, &returned);
  const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
  if (!RevertToSelf()) std::abort();
  Check(error, "package-path-query");
  const wchar_t* end = std::wmemchr(path, L'\0', std::size(path));
  Require(queried && returned && returned <= std::size(path) && end && end != path,
      "package-path-bounds", ERROR_INVALID_DATA);
  const std::wstring relative = L"AppContainerNamedObjects\\" + SidText(package);
  const std::wstring absolute = L"\\Sessions\\" + std::to_wstring(session) + L"\\" + relative;
  const std::wstring actual(path, static_cast<size_t>(end - path));
  Require(actual == relative || actual == absolute, "package-path-exact-session-sid", ERROR_INVALID_NAME);
  return absolute;
}
void RequireAbsentPackage(const std::wstring& path, DWORD* observed) {
  DWORD status = 0;
  const Handle existing = OpenSessionDirectory(path, 0x00020003, nullptr, &status);
  Require(!existing && (status == 0xc0000034 || status == 0xc000003a),
      "fresh-package-directory-absent", ERROR_ALREADY_EXISTS);
  *observed = 1;
}
template<size_t Size>
void CopyNamespaceText(const std::wstring& text, wchar_t (&target)[Size]) {
  Require(text.size() < Size, "package-evidence-text-bound", ERROR_MORE_DATA);
  std::wmemcpy(target, text.c_str(), text.size() + 1);
}
template<size_t Size>
void CopyObjectSecurity(PSECURITY_DESCRIPTOR descriptor, wchar_t (&target)[Size]) {
  wchar_t* text = nullptr;
  ULONG returned = 0;
  Require(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1,
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
      &text, &returned), "package-security-sddl");
  const Local owned(text);
  Require(text && returned && returned <= Size, "package-security-sddl-bound", ERROR_MORE_DATA);
  CopyNamespaceText(std::wstring(text), target);
}
void VerifyPackageLeafPolicy(PSECURITY_DESCRIPTOR descriptor, const storage::AuthorityLaunchSpec& spec,
                             bool pipe = false) {
  PSID owner = nullptr, group = nullptr;
  PACL dacl = nullptr, label = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  Require(GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) && owner &&
      IsWellKnownSid(owner, WinLocalSystemSid) &&
      GetSecurityDescriptorGroup(descriptor, &group, &defaulted) && group &&
      IsWellKnownSid(group, WinLocalSystemSid), "package-leaf-system-owner-and-group", ERROR_INVALID_OWNER);
  Require(GetSecurityDescriptorControl(descriptor, &control, &revision) &&
      (control & (SE_DACL_PROTECTED | SE_SACL_PROTECTED)) == (SE_DACL_PROTECTED | SE_SACL_PROTECTED) &&
      GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present && dacl &&
      IsValidAcl(dacl) && dacl->AceCount == 6, "package-leaf-protected-exact-acl", ERROR_INVALID_ACL);
  const DWORD masks[] = {pipe ? 0x001f01ffu : 0x000f000fu, GENERIC_ALL,
      pipe ? 0x001f01ffu : 0x000f000fu, GENERIC_ALL,
      pipe ? 0x001201ffu : 0x0002000fu, GENERIC_ALL};
  const BYTE flags[] = {0, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE,
      0, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE, 0,
      OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE};
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    Require(GetAce(dacl, i, &raw), "package-leaf-actual-ace");
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    Require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == flags[i] &&
        ace->Mask == masks[i] && IsValidSid(sid) &&
        (i < 2 ? IsWellKnownSid(sid, WinLocalSystemSid) :
         EqualSid(sid, i < 4 ? spec.desktop_user : spec.capabilities.AppContainerSid)),
        "package-leaf-exact-trustee-mask-and-inheritance", ERROR_INVALID_ACL);
  }
  Require(GetSecurityDescriptorSacl(descriptor, &present, &label, &defaulted) && present && label &&
      IsValidAcl(label) && label->AceCount == 1, "package-leaf-explicit-mandatory-label", ERROR_INVALID_ACL);
  void* raw = nullptr;
  Require(GetAce(label, 0, &raw), "package-leaf-label-ace");
  const auto* ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(raw);
  Require(ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE && !ace->Header.AceFlags &&
      ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP &&
      IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinLowLabelSid),
      "package-leaf-low-no-write-up-label", ERROR_INVALID_ACL);
}

void CreatePackageChildren(const std::wstring& path, HANDLE guest, HANDLE other, HANDLE desktop,
    PackageLeafEvidence* evidence) {
  DWORD status = 0;
  const Handle parent = OpenSessionDirectory(path, 0x0000000f, guest, &status);
  Require(status == 0 && parent, "package-leaf-own-create-directory-handle", ERROR_ACCESS_DENIED);
  // EVENT_TYPE is a native four-byte enum: NotificationEvent=0. Keep this local
  // ABI declaration independent of WDK-only header types in the host fixture.
  enum class NativeEventType : int { notification = 0 };
  using CreateNativeEvent = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, NativeEventType, BOOLEAN);
  using CreateNativeDirectory = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
  using OpenNative = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
  const HMODULE native = GetModuleHandleW(L"ntdll.dll");
  const auto event_create = reinterpret_cast<CreateNativeEvent>(GetProcAddress(native, "NtCreateEvent"));
  const auto directory_create = reinterpret_cast<CreateNativeDirectory>(GetProcAddress(native, "NtCreateDirectoryObject"));
  const auto event_open = reinterpret_cast<OpenNative>(GetProcAddress(native, "NtOpenEvent"));
  Require(event_create && directory_create && event_open, "package-child-create-open-exports", ERROR_PROC_NOT_FOUND);
  wchar_t event_name[] = L"GesherFixtureEvent", directory_name[] = L"GesherFixtureDirectory";
  UNICODE_STRING name{};
  name.Buffer = event_name;
  name.Length = static_cast<USHORT>(sizeof(event_name) - sizeof(wchar_t));
  name.MaximumLength = sizeof(event_name);
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent.get();
  attributes.ObjectName = &name;
  attributes.Attributes = 0x00001000;  // No OPENIF, INHERIT or PERMANENT.
  HANDLE event_raw = nullptr, directory_raw = nullptr;
  Require(ImpersonateLoggedOnUser(guest), "package-child-create-lowbox-context");
  // Owning a protected child requires operational access, not Windows
  // permission to change its owner or DACL. Windows can remove those rights
  // from an AppContainer creator's handle even when it creates the object.
  const NTSTATUS event_status = event_create(&event_raw, kChildEventUseAccess,
      &attributes, NativeEventType::notification, FALSE);
  const BOOL signaled = event_status == 0 && event_raw ? SetEvent(event_raw) : FALSE;
  const DWORD signal_error = event_status == 0 && event_raw && !signaled ? GetLastError() : ERROR_SUCCESS;
  const DWORD waited = signaled ? WaitForSingleObject(event_raw, 0) : WAIT_FAILED;
  const DWORD wait_error = signaled && waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  name.Buffer = directory_name;
  name.Length = static_cast<USHORT>(sizeof(directory_name) - sizeof(wchar_t));
  name.MaximumLength = sizeof(directory_name);
  const NTSTATUS directory_status = directory_create(&directory_raw, kChildDirectoryUseAccess, &attributes);
  if (!RevertToSelf()) std::abort();
  Handle event(event_raw), directory(directory_raw);
  auto& observed = evidence->event_creation;
  observed.called = 1;
  observed.status = static_cast<DWORD>(event_status);
  observed.opened = event ? 1 : 0;
  observed.signaled = signaled != FALSE;
  observed.signal_error = signal_error;
  observed.waited = waited;
  observed.wait_error = wait_error;
  if (event) {
    observed.type_matches = SessionObjectText(event.get(), ObjectTypeInformation) == L"Event";
    observed.name_matches = SessionObjectText(event.get(), static_cast<OBJECT_INFORMATION_CLASS>(1)) ==
        path + L"\\" + event_name;
    observed.granted_access = SessionObjectAccess(event.get());
  }
  Require(observed.status == 0 && observed.opened && observed.signaled &&
      observed.waited == WAIT_OBJECT_0 && observed.type_matches && observed.name_matches &&
      observed.granted_access == kChildEventUseAccess,
      "package-leaf-actual-event-creation-use-and-inherited-access", ERROR_ACCESS_DENIED);
  evidence->object_created = 1;
  Require(directory_status == 0 && directory &&
      SessionObjectText(directory.get(), ObjectTypeInformation) == L"Directory" &&
      SessionObjectText(directory.get(), static_cast<OBJECT_INFORMATION_CLASS>(1)) == path + L"\\" + directory_name &&
      SessionObjectAccess(directory.get()) == kChildDirectoryUseAccess,
      "package-leaf-actual-subdirectory-creation-and-inherited-access", ERROR_ACCESS_DENIED);
  evidence->subdirectory_created = 1;
  // Temporary named objects would disappear at last close. Independent
  // authority-side read-only pins keep these exact objects alive while the
  // creator handles are closed and each actual subject tries to reopen them.
  HANDLE event_pin_raw = nullptr, directory_pin_raw = nullptr;
  Require(DuplicateHandle(GetCurrentProcess(), event.get(), GetCurrentProcess(), &event_pin_raw,
      READ_CONTROL | SYNCHRONIZE, FALSE, 0), "package-child-authority-event-pin");
  const Handle event_pin(event_pin_raw);
  Require(DuplicateHandle(GetCurrentProcess(), directory.get(), GetCurrentProcess(), &directory_pin_raw,
      0x00020003, FALSE, 0), "package-child-authority-directory-pin");
  const Handle directory_pin(directory_pin_raw);
  const Local event_sd = SessionObjectSecurity(event_pin.get()), directory_sd = SessionObjectSecurity(directory_pin.get());
  CopyObjectSecurity(event_sd.get(), evidence->event_security);
  CopyObjectSecurity(directory_sd.get(), evidence->directory_security);
  event.reset(); directory.reset();

  const std::wstring child_names[] = {path + L"\\" + event_name, path + L"\\" + directory_name};
  const HANDLE pins[] = {event_pin.get(), directory_pin.get()}, subjects[] = {guest, other, desktop};
  const DWORD use_rights[] = {kChildEventUseAccess, kChildDirectoryUseAccess};
  const DWORD all_rights[] = {EVENT_ALL_ACCESS, kChildDirectoryAllAccess};
  const OpenNative openers[] = {event_open, SessionDirectoryOpen()};
  const auto reopen = [&](size_t kind, DWORD access, HANDLE subject, DWORD* observed) {
    UNICODE_STRING child_name{};
    child_name.Buffer = const_cast<wchar_t*>(child_names[kind].data());
    child_name.Length = static_cast<USHORT>(child_names[kind].size() * sizeof(wchar_t));
    child_name.MaximumLength = child_name.Length;
    OBJECT_ATTRIBUTES child_attributes{};
    child_attributes.Length = sizeof(child_attributes);
    child_attributes.ObjectName = &child_name;
    child_attributes.Attributes = 0x00001000;
    Require(ImpersonateLoggedOnUser(subject), "package-child-reopen-subject");
    HANDLE raw = nullptr;
    const NTSTATUS opened = openers[kind](&raw, access, &child_attributes);
    if (!RevertToSelf()) std::abort();
    *observed = static_cast<DWORD>(opened);
    return Handle(raw);
  };
  for (size_t kind = 0; kind < 2; ++kind) {
    for (size_t actor = 0; actor < 3; ++actor) {
      auto& row = evidence->child_opens[kind][actor]; row.called = 1;
      const DWORD requested = actor == 0 ? use_rights[kind] : all_rights[kind];
      const Handle opened = reopen(kind, requested, subjects[actor], &row.status);
      row.opened = static_cast<DWORD>(static_cast<bool>(opened));
      if (actor == 0) {
        Require(row.status == 0 && opened && SameSessionObject(opened.get(), pins[kind]) &&
            SessionObjectAccess(opened.get()) == requested,
            "package-child-own-reopen-exact-object", ERROR_ACCESS_DENIED);
        Require(ImpersonateLoggedOnUser(guest), "package-child-use-reopened-lowbox-context");
        BOOL used = FALSE;
        HANDLE nested_raw = nullptr;
        if (kind == 0) {
          used = ResetEvent(opened.get()) && SetEvent(opened.get()) && WaitForSingleObject(opened.get(), 0) == WAIT_OBJECT_0;
        } else {
          wchar_t nested_name[] = L"NestedFixtureEvent";
          UNICODE_STRING nested{static_cast<USHORT>(sizeof(nested_name) - sizeof(wchar_t)),
              static_cast<USHORT>(sizeof(nested_name)), nested_name};
          OBJECT_ATTRIBUTES nested_attributes{};
          nested_attributes.Length = sizeof(nested_attributes);
          nested_attributes.RootDirectory = opened.get();
          nested_attributes.ObjectName = &nested;
          nested_attributes.Attributes = 0x00001000;
          const NTSTATUS nested_status = event_create(&nested_raw, kChildEventUseAccess, &nested_attributes,
              NativeEventType::notification, FALSE);
          used = nested_status == 0 && nested_raw && SetEvent(nested_raw) && WaitForSingleObject(nested_raw, 0) == WAIT_OBJECT_0;
        }
        if (!RevertToSelf()) std::abort();
        const Handle nested(nested_raw);
        Require(used != FALSE, "package-child-actual-use-after-own-reopen", ERROR_ACCESS_DENIED);
        evidence->child_used[kind] = 1;
      } else {
        Require(!opened && row.status == 0xc0000022, "package-child-other-subject-reopen-denied", ERROR_ACCESS_DENIED);
        const DWORD masks[][5] = {{EVENT_MODIFY_STATE, SYNCHRONIZE, DELETE, WRITE_DAC, WRITE_OWNER},
            {0x0004, 0x0008, DELETE, WRITE_DAC, WRITE_OWNER}};
        for (DWORD access : masks[kind]) {
          DWORD denied = 0;
          const Handle forbidden = reopen(kind, access, subjects[actor], &denied);
          Require(!forbidden && denied == 0xc0000022,
              "package-child-other-subject-operation-and-mutation-denied", ERROR_ACCESS_DENIED);
          ++evidence->child_denials[kind][actor - 1];
        }
      }
    }
  }
}

std::wstring PackagePipePath(const storage::AuthorityLaunchSpec& spec) {
  return L"\\Device\\NamedPipe\\Sessions\\" + std::to_wstring(spec.desktop_session) +
      L"\\AppContainerNamedObjects\\" + SidText(spec.capabilities.AppContainerSid);
}
Handle OpenPackagePipe(const storage::AuthorityLaunchSpec& spec, ACCESS_MASK access,
                       HANDLE token, DWORD* result) {
  using Open = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
      PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  const auto open = reinterpret_cast<Open>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  Require(open != nullptr, "pipe-leaf-native-open-export", ERROR_PROC_NOT_FOUND);
  std::wstring path = PackagePipePath(spec);
  UNICODE_STRING name{};
  name.Buffer = path.data();
  name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
  name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(wchar_t));
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes); attributes.ObjectName = &name;
  attributes.Attributes = 0x1040;  // DONT_REPARSE | CASE_INSENSITIVE; never OPENIF.
  IO_STATUS_BLOCK iosb{};
  HANDLE raw = nullptr;
  if (token) Require(ImpersonateLoggedOnUser(token), "pipe-leaf-open-actual-subject");
  const NTSTATUS status = open(&raw, access, &attributes, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 1 /* FILE_OPEN */, 1 /* FILE_DIRECTORY_FILE */, nullptr, 0);
  if (token && !RevertToSelf()) std::abort();
  *result = static_cast<DWORD>(status);
  return Handle(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
}
void ObservePackagePipe(const storage::AuthorityLaunch& launch, const storage::AuthorityLaunchSpec& spec,
                        HANDLE guest, HANDLE other, HANDLE desktop, PackageLeafEvidence* evidence) {
  auto& observed = evidence->pipe_pin;
  observed.called = 1;
  const HANDLE pipe = launch.package_pipe_objects();
  observed.present = pipe != nullptr;
  if (pipe) {
    observed.info_ok = GetHandleInformation(pipe, &observed.flags) != FALSE;
    observed.info_status = observed.info_ok ? ERROR_SUCCESS : GetLastError();
    if (observed.info_ok) {
      observed.granted_access = SessionObjectAccess(pipe);
      observed.type_matches = SessionObjectText(pipe, ObjectTypeInformation) == L"File";
      const auto actual = SessionObjectText(pipe, static_cast<OBJECT_INFORMATION_CLASS>(1));
      CopyNamespaceText(actual, observed.name);
      // A secure NPFS directory can report the device name alone. This
      // checks the namespace only. The exact-path open and package-specific
      // security below, and the shared live DACL control on reuse, identify the leaf.
      observed.device_name_matches = actual == L"\\Device\\NamedPipe" || actual == PackagePipePath(spec);
    }
  }
  Require(observed.present && observed.info_ok && !(observed.flags & HANDLE_FLAG_INHERIT) &&
      observed.granted_access == (READ_CONTROL | SYNCHRONIZE) && observed.type_matches &&
      observed.device_name_matches,
      "pipe-leaf-launch-owns-exact-readonly-pin", ERROR_INVALID_HANDLE);
  const Local descriptor = SessionObjectSecurity(pipe);
  VerifyPackageLeafPolicy(descriptor.get(), spec, true);
  DWORD status = 0;
  const Handle fresh = OpenPackagePipe(spec, READ_CONTROL | SYNCHRONIZE, nullptr, &status);
  Require(status == 0 && fresh && fresh.get() != pipe &&
      SessionObjectAccess(fresh.get()) == (READ_CONTROL | SYNCHRONIZE),
      "pipe-leaf-independent-exact-path-open", ERROR_NOT_SAME_OBJECT);
  const Local fresh_descriptor = SessionObjectSecurity(fresh.get());
  VerifyPackageLeafPolicy(fresh_descriptor.get(), spec, true);
  evidence->pipe_pin_verified = 1;
  evidence->pipe_policy_verified = 1;
  const Handle own = OpenPackagePipe(spec, READ_CONTROL, guest, &status);
  Require(status == 0 && own, "pipe-leaf-actual-package-read-positive", ERROR_ACCESS_DENIED);
  evidence->pipe_positive = 1;
  for (const HANDLE subject : {guest, other, desktop}) {
    for (const DWORD access : {DELETE, WRITE_DAC, WRITE_OWNER}) {
      const Handle denied = OpenPackagePipe(spec, access, subject, &status);
      Require(!denied && status == 0xc0000022, "pipe-leaf-actual-subject-mutation-denied", ERROR_ACCESS_DENIED);
      ++evidence->pipe_denials;
    }
  }
  for (const HANDLE subject : {other, desktop}) {
    const Handle denied = OpenPackagePipe(spec, READ_CONTROL, subject, &status);
    Require(!denied && status == 0xc0000022, "pipe-leaf-other-subject-read-denied", ERROR_ACCESS_DENIED);
    ++evidence->pipe_denials;
  }
}

void ObserveProtectedPolicy(const std::wstring& parent, const storage::AuthorityLaunchSpec& spec,
                            HANDLE private_token, HANDLE guest, HANDLE other, PackageLeafEvidence* evidence) {
  const std::wstring root = parent + L"\\policy-fixture";
  const std::wstring policy_root = root + L"\\system\\storage-policy";
  const std::wstring own_directory = policy_root + L"\\fixture.own";
  const std::wstring other_directory = policy_root + L"\\fixture.other";
  const std::wstring own = own_directory + L"\\policy", replaced = own_directory + L"\\next";
  const std::wstring foreign = other_directory + L"\\policy";
  struct Cleanup {
    std::vector<std::wstring> files, directories;
    ~Cleanup() {
      for (const auto& path : files) DeleteFileW(path.c_str());
      for (auto it = directories.rbegin(); it != directories.rend(); ++it) RemoveDirectoryW(it->c_str());
    }
  } cleanup{{own, replaced, foreign}, {root, root + L"\\system", policy_root, own_directory, other_directory}};
  const std::wstring dacl = L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + SidText(spec.desktop_user) + L")";
  PSECURITY_DESCRIPTOR raw = nullptr;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1,
      &raw, nullptr), "policy-fixture-directory-security");
  const Local security(raw);
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), raw, FALSE};
  Require(CreateDirectoryW(root.c_str(), &attributes), "policy-fixture-unique-root");
  raw = nullptr;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1,
      &raw, nullptr), "policy-fixture-low-label");
  const Local low_label(raw);
  BOOL present = FALSE, defaulted = FALSE;
  PACL label = nullptr;
  Require(GetSecurityDescriptorSacl(raw, &present, &label, &defaulted) && present && label,
      "policy-fixture-label-present");
  // Lower only our newly created directory's integrity, using LABEL information;
  // no SACL audit privilege or process privilege change is involved.
  std::wstring writable_root = root;
  Check(SetNamedSecurityInfoW(writable_root.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
      nullptr, nullptr, nullptr, label), "policy-fixture-set-inheritable-low-label");
  for (size_t i = 1; i < cleanup.directories.size(); ++i) {
    // Match system_server's common TokenUser ownership on each package's
    // directory. The parent trust boundary itself remains SYSTEM-created.
    if (i >= 3) Require(ImpersonateLoggedOnUser(private_token), "policy-fixture-private-directory-creator");
    const BOOL created = CreateDirectoryW(cleanup.directories[i].c_str(), nullptr);
    const DWORD error = created ? ERROR_SUCCESS : GetLastError();
    if (i >= 3 && !RevertToSelf()) std::abort();
    Check(error, "policy-fixture-directory");
  }
  const auto publish = [&](const std::wstring& path, const char* content) {
    Require(ImpersonateLoggedOnUser(private_token), "policy-fixture-private-file-creator");
    const Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    const DWORD error = file.get() == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (!RevertToSelf()) std::abort();
    Check(error, "policy-fixture-publish-open");
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor_raw = nullptr;
    Check(GetSecurityInfo(file.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor_raw), "policy-fixture-file-owner");
    const Local descriptor(descriptor_raw);
    Require(owner && EqualSid(owner, spec.desktop_user), "policy-fixture-real-common-private-owner", ERROR_INVALID_OWNER);
    const DWORD length = static_cast<DWORD>(std::strlen(content));
    DWORD written = 0;
    Require(WriteFile(file.get(), content, length, &written, nullptr) && written == length && FlushFileBuffers(file.get()),
        "policy-fixture-publish-bytes");
  };
  publish(own, "own-initial"); publish(foreign, "other-policy");
  Check(storage::GrantProtectedStoragePolicy(root, L"fixture.own", spec.capabilities.AppContainerSid),
      "policy-production-own-directory-grant");
  Require(storage::GrantProtectedStoragePolicy(root, L"../fixture.other", spec.capabilities.AppContainerSid)
      == ERROR_INVALID_PARAMETER, "policy-production-path-override-denied", ERROR_INVALID_DATA);
  evidence->policy_bad_path_denied = 1;
  const auto open_as = [&](HANDLE token, const std::wstring& path, DWORD access, DWORD* status) {
    Require(ImpersonateLoggedOnUser(token), "policy-fixture-actual-subject");
    HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    *status = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (!RevertToSelf()) std::abort();
    return Handle(file == INVALID_HANDLE_VALUE ? nullptr : file);
  };
  const auto read_own = [&](const char* expected) {
    DWORD status = 0;
    const Handle file = open_as(guest, own, GENERIC_READ | READ_CONTROL, &status);
    Require(status == ERROR_SUCCESS && file, "policy-actual-lowbox-read-positive", status);
    std::array<char, 64> bytes{};
    DWORD count = 0;
    Require(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
        std::string_view(bytes.data(), count) == expected, "policy-actual-lowbox-read-published-bytes");
    PSECURITY_DESCRIPTOR descriptor_raw = nullptr;
    Check(GetSecurityInfo(file.get(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor_raw), "policy-actual-file-label");
    const Local descriptor(descriptor_raw);
    PACL actual = nullptr;
    Require(GetSecurityDescriptorSacl(descriptor_raw, &present, &actual, &defaulted) && present && actual && actual->AceCount == 1,
        "policy-file-low-label-present", ERROR_INVALID_ACL);
    void* ace_raw = nullptr;
    Require(GetAce(actual, 0, &ace_raw), "policy-file-label-ace");
    const auto* ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(ace_raw);
    Require(ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE &&
        IsWellKnownSid(const_cast<DWORD*>(&ace->SidStart), WinLowLabelSid),
        "policy-write-denials-not-confounded-by-higher-integrity", ERROR_INVALID_ACL);
  };
  read_own("own-initial"); evidence->policy_own_read = 1;
  DWORD status = 0;
  const Handle foreign_read = open_as(guest, foreign, GENERIC_READ, &status);
  Require(!foreign_read && status == ERROR_ACCESS_DENIED, "policy-other-package-metadata-read-denied", status);
  const Handle other_read = open_as(other, own, GENERIC_READ, &status);
  Require(!other_read && status == ERROR_ACCESS_DENIED, "policy-other-subject-own-metadata-read-denied", status);
  evidence->policy_other_denied = 2;
  constexpr DWORD modification_access[] = {FILE_WRITE_DATA, FILE_APPEND_DATA, DELETE, WRITE_DAC, WRITE_OWNER};
  for (const auto& path : {own, own_directory, foreign, other_directory}) {
    for (const DWORD access : modification_access) {
      const Handle denied = open_as(guest, path, access, &status);
      Require(!denied && status == ERROR_ACCESS_DENIED, "policy-actual-lowbox-modification-denied", status);
      ++evidence->policy_write_denials;
    }
  }
  publish(replaced, "own-replacement");
  Require(MoveFileExW(replaced.c_str(), own.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
      "policy-fixture-atomic-replacement");
  read_own("own-replacement"); evidence->policy_replacement = 1;
}

void ObservePackageLeaf(const std::wstring& path, const storage::AuthorityLaunch& launch,
    const storage::AuthorityLaunchSpec& spec, HANDLE guest, HANDLE other, HANDLE desktop,
    PackageLeafEvidence* evidence) {
  CopyNamespaceText(path, evidence->path);
  DWORD status = 0, flags = 0;
  const Handle pinned = OpenSessionDirectory(path, 0x00020003, nullptr, &status);
  Require(status == 0 && pinned && GetHandleInformation(pinned.get(), &flags) &&
      !(flags & HANDLE_FLAG_INHERIT) && SessionObjectAccess(pinned.get()) == 0x00020003 &&
      SessionObjectText(pinned.get(), ObjectTypeInformation) == L"Directory" &&
      SessionObjectText(pinned.get(), static_cast<OBJECT_INFORMATION_CLASS>(1)) == path,
      "package-leaf-exact-pinned-directory", ERROR_NOT_SAME_OBJECT);
  const Local descriptor = SessionObjectSecurity(pinned.get());
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  Require(GetSecurityDescriptorControl(descriptor.get(), &control, &revision),
      "package-leaf-control");
  evidence->control = control;
  CopyObjectSecurity(descriptor.get(), evidence->security);
  evidence->captured = 1;
  VerifyPackageLeafPolicy(descriptor.get(), spec);
  evidence->policy_verified = 1;
  Require(launch.package_objects() && GetHandleInformation(launch.package_objects(), &flags) &&
      !(flags & HANDLE_FLAG_INHERIT) && SessionObjectAccess(launch.package_objects()) == 0x00020003 &&
      SameSessionObject(pinned.get(), launch.package_objects()),
      "package-leaf-launch-owns-exact-readonly-pin", ERROR_NOT_SAME_OBJECT);
  evidence->launch_pin_verified = 1;
  bool isolated = true;
  const HANDLE subjects[] = {guest, other, desktop};
  for (size_t actor = 0; actor < std::size(subjects); ++actor) {
    for (size_t i = 0; i < std::size(kSessionMutationMasks); ++i) {
      auto& row = evidence->opens[actor][i];
      row.called = 1;
      const Handle opened = OpenSessionDirectory(path, kSessionMutationMasks[i], subjects[actor], &row.status);
      row.opened = static_cast<DWORD>(static_cast<bool>(opened));
      if (opened) {
        Require(SameSessionObject(pinned.get(), opened.get()) &&
            SessionObjectAccess(opened.get()) == kSessionMutationMasks[i],
            "package-leaf-open-exact-object-right", ERROR_NOT_SAME_OBJECT);
        if (actor != 0 || i >= 2) isolated = false;
      } else {
        Require(row.status == 0xc0000022, "package-leaf-open-denial-status", ERROR_INVALID_DATA);
      }
    }
  }
  // The package creates children but cannot mutate this authority-owned root.
  // Check actual lowbox access; a common TokenUser is not isolation evidence.
  Require(evidence->opens[0][0].opened && evidence->opens[0][1].opened,
      "package-leaf-own-create-positive-control", ERROR_ACCESS_DENIED);
  Require(isolated, "package-leaf-own-standard-and-other-desktop-mutations-denied", ERROR_ACCESS_DENIED);
  for (HANDLE subject : subjects) {
    DWORD denied = 0;
    const Handle forbidden = OpenSessionDirectory(path, ACCESS_SYSTEM_SECURITY, subject, &denied);
    Require(!forbidden && (denied == 0xc0000022 || denied == 0xc0000061),
        "package-leaf-lowbox-and-desktop-sacl-mutation-denied", ERROR_ACCESS_DENIED);
    ++evidence->sacl_mutation_denials;
  }
  CreatePackageChildren(path, guest, other, desktop, evidence);
  ObservePackagePipe(launch, spec, guest, other, desktop, evidence);
  evidence->status = ERROR_SUCCESS;
}

void ObservePackageSaclReuse(HANDLE private_token, const storage::AuthorityLaunchSpec& original,
    const storage::AuthorityLaunch& first, const std::wstring& path, PackageLeafEvidence* evidence) {
  // Mutate only this fixture-owned leaf. The production finalizer must never
  // adopt a preexisting unprotected descriptor or erase an unseen audit ACE.
  const Handle writable = OpenFixtureSacl(path);
  Require(SameSessionObject(writable.get(), first.package_objects()),
      "fixture-sacl-exact-retained-object", ERROR_NOT_SAME_OBJECT);
  const Local before = FullSaclSecurity(writable.get());
  VerifyPackageLeafPolicy(before.get(), original);
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  PACL original_sacl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  Require(GetSecurityDescriptorControl(before.get(), &control, &revision) &&
      GetSecurityDescriptorSacl(before.get(), &present, &original_sacl, &defaulted) && present && original_sacl,
      "fixture-sacl-original-complete-policy");
  const auto set = SecuritySetter();
  FixtureSaclRestore restore{set, writable.get(), before.get()};
  constexpr WORD control_bits = SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED | SE_SACL_AUTO_INHERIT_REQ;
  constexpr WORD wrapper_reset = SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED;
  SECURITY_DESCRIPTOR unprotected{};
  Require(InitializeSecurityDescriptor(&unprotected, SECURITY_DESCRIPTOR_REVISION) &&
      SetSecurityDescriptorSacl(&unprotected, TRUE, original_sacl, FALSE) &&
      SetSecurityDescriptorControl(&unprotected, control_bits,
          static_cast<WORD>(control & control_bits & ~wrapper_reset)), "fixture-sacl-unprotected-descriptor");
  restore.dirty = true;
  {
    // Reproduce the actual wrapper's LABEL-only update on this fixture object.
    // This separate WRITE_OWNER handle is never used by production resealing.
    DWORD opened = 0;
    const Handle label_writer = OpenSessionDirectory(path, WRITE_OWNER, nullptr, &opened);
    Require(opened == 0 && label_writer && SameSessionObject(label_writer.get(), first.package_objects()) &&
        SessionObjectAccess(label_writer.get()) == WRITE_OWNER,
        "fixture-sacl-exact-label-control-handle", ERROR_ACCESS_DENIED);
    Require(set(label_writer.get(), LABEL_SECURITY_INFORMATION, &unprotected) == 0,
        "fixture-sacl-apply-wrapper-label-update", ERROR_ACCESS_DENIED);
  }
  const Local unprotected_before = FullSaclSecurity(writable.get());
  SECURITY_DESCRIPTOR_CONTROL unprotected_control{};
  Require(GetSecurityDescriptorControl(unprotected_before.get(), &unprotected_control, &revision) &&
      unprotected_control == static_cast<WORD>(control & ~wrapper_reset),
      "fixture-sacl-unprotected-change-observed", ERROR_INVALID_SECURITY_DESCR);
  storage::AuthorityNamespaceCreateResult observed;
  auto spec = original;
  spec.expected_package_objects = first.package_objects();
  spec.namespace_create_result = &observed;
  std::unique_ptr<storage::AuthorityLaunch> child;
  const char* stage = "package-reuse.unprotected-sacl";
  DWORD rejected = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  const Local unprotected_after = FullSaclSecurity(writable.get());
  SECURITY_DESCRIPTOR_CONTROL unchanged_control{};
  const bool unprotected_unchanged = GetSecurityDescriptorControl(unprotected_after.get(), &unchanged_control, &revision) &&
      unchanged_control == unprotected_control;
  Require(restore.Restore() == 0, "fixture-sacl-restore-unprotected-control", ERROR_ACCESS_DENIED);
  Require(rejected == ERROR_ACCESS_DENIED && !child && stage &&
      std::strcmp(stage, "lowbox.verify-package-namespace") == 0 && unprotected_unchanged &&
      observed.reseal.stage == storage::AuthorityNamespaceSealStage::kNotAttempted && !observed.reseal.write_called,
      "package-reuse-preexisting-unprotected-sacl-is-never-repaired", ERROR_INVALID_DATA);
  evidence->unprotected_reuse_rejected = 1;

  PSECURITY_DESCRIPTOR audit_raw = nullptr;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"S:P(AU;SAFA;0x00000001;;;WD)(ML;;NW;;;LW)", SDDL_REVISION_1, &audit_raw, nullptr),
      "fixture-sacl-audit-descriptor");
  const Local audit(audit_raw);
  WORD audit_control_request = static_cast<WORD>(control & control_bits);
  if (control & SE_SACL_AUTO_INHERITED) audit_control_request |= SE_SACL_AUTO_INHERIT_REQ;
  Require(SetSecurityDescriptorControl(audit.get(), control_bits, audit_control_request),
      "fixture-sacl-audit-original-control");
  PACL audit_acl = nullptr;
  Require(GetSecurityDescriptorSacl(audit.get(), &present, &audit_acl, &defaulted) && present && audit_acl &&
      audit_acl->AceCount == 2, "fixture-sacl-audit-and-label");
  const auto audit_bytes = SessionAclBytes(audit_acl);
  restore.dirty = true;
  Require(set(writable.get(), SACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION, audit.get()) == 0,
      "fixture-sacl-apply-audit-entry", ERROR_ACCESS_DENIED);
  const Local audit_before = FullSaclSecurity(writable.get());
  PACL actual_audit = nullptr;
  SECURITY_DESCRIPTOR_CONTROL audit_control{};
  Require(GetSecurityDescriptorControl(audit_before.get(), &audit_control, &revision) && audit_control == control &&
      GetSecurityDescriptorSacl(audit_before.get(), &present, &actual_audit, &defaulted) && present && actual_audit &&
      SessionAclBytes(actual_audit) == audit_bytes, "fixture-sacl-audit-change-observed", ERROR_INVALID_ACL);
  // This is the independent control for the original label-only read: it still
  // looks valid even though the privileged full read exposes the extra ACE.
  const Local label_only = SessionObjectSecurity(writable.get());
  VerifyPackageLeafPolicy(label_only.get(), original);
  observed = {};
  stage = "package-reuse.audit-sacl";
  rejected = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  const Local audit_after = FullSaclSecurity(writable.get());
  PACL retained_audit = nullptr;
  const bool audit_preserved = GetSecurityDescriptorSacl(audit_after.get(), &present, &retained_audit, &defaulted) &&
      present && retained_audit && SessionAclBytes(retained_audit) == audit_bytes;
  Require(restore.Restore() == 0, "fixture-sacl-restore-original-audit-policy", ERROR_ACCESS_DENIED);
  Require(rejected == ERROR_ACCESS_DENIED && !child && stage &&
      std::strcmp(stage, "lowbox.reseal-package-sacl") == 0 &&
      observed.directory_policy.stage == storage::AuthorityNamespacePolicyStage::kComplete &&
      observed.reseal.stage == storage::AuthorityNamespaceSealStage::kFullPolicy &&
      observed.reseal.before_policy.stage == storage::AuthorityNamespacePolicyStage::kPolicy &&
      observed.reseal.before_policy.requested_information == 0x1f && !observed.reseal.write_called &&
      observed.reseal.after_policy.stage == storage::AuthorityNamespacePolicyStage::kNotAttempted &&
      observed.reseal.close_called == 1 && observed.reseal.close_status == ERROR_SUCCESS &&
      observed.reseal.privilege_restore_status == ERROR_SUCCESS &&
      observed.reseal.privilege_before == observed.reseal.privilege_restored &&
      observed.reseal.context_restore_status == ERROR_SUCCESS && audit_preserved,
      "package-reuse-extra-audit-entry-rejected-without-erasure", ERROR_INVALID_DATA);
  evidence->audit_reuse_rejected = 1;
  evidence->audit_preserved = 1;
  const Local after = FullSaclSecurity(writable.get());
  VerifyPackageLeafPolicy(after.get(), original);
  SECURITY_DESCRIPTOR_CONTROL after_control{};
  Require(GetSecurityDescriptorControl(after.get(), &after_control, &revision) && after_control == control,
      "fixture-sacl-exact-original-control-restored", ERROR_INVALID_SECURITY_DESCR);
  evidence->sacl_fixture_restored = 1;
}

void ObservePackageReuse(HANDLE private_token, const storage::AuthorityLaunchSpec& original,
    const storage::AuthorityLaunch& first, const std::wstring& path, PackageLeafEvidence* evidence) {
  HANDLE raw = nullptr;
  Require(DuplicateHandle(GetCurrentProcess(), first.package_objects(), GetCurrentProcess(), &raw,
      0x00020003, FALSE, 0), "package-reuse-independent-profile-pin");
  Handle profile_pin(raw);
  auto spec = original;
  spec.expected_package_objects = profile_pin.get();
  std::unique_ptr<storage::AuthorityLaunch> child;
  const char* stage = "package-reuse.launch";
  DWORD created = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  Check(created, stage);
  Require(child && child->package_objects() != profile_pin.get() &&
      SessionObjectAccess(child->package_objects()) == 0x00020003 &&
      SameSessionObject(child->package_objects(), first.package_objects()),
      "package-reuse-launch-owns-independent-readonly-pin", ERROR_NOT_SAME_OBJECT);
  Require(child->package_pipe_objects() && child->package_pipe_objects() != first.package_pipe_objects() &&
      SessionObjectAccess(child->package_pipe_objects()) == (READ_CONTROL | SYNCHRONIZE),
      "pipe-reuse-independent-readonly-launch-pin", ERROR_INVALID_HANDLE);
  const Local pipe_reused = SessionObjectSecurity(child->package_pipe_objects());
  VerifyPackageLeafPolicy(pipe_reused.get(), spec, true);
  evidence->pipe_reused = 1;
  evidence->expected_reuse = 1;
  profile_pin.reset();
  Require(SameSessionObject(child->package_objects(), first.package_objects()) &&
      WaitForSingleObject(child->process(), 0) == WAIT_TIMEOUT,
      "package-reuse-profile-destruction-does-not-drop-launch-pin", ERROR_NOT_SAME_OBJECT);
  evidence->independent_pin_lifetime = 1;
  Check(child->Stop(10000), "package-reuse-stop-suspended-child");
  child.reset();

  // A fresh Profile has no old pin; complete authentication of an existing
  // SYSTEM-owned exact-policy leaf still permits a same-package restart.
  spec.expected_package_objects = nullptr;
  stage = "package-reuse.new-profile";
  created = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  Check(created, stage);
  Require(child && SameSessionObject(child->package_objects(), first.package_objects()),
      "package-reuse-null-pin-still-authenticates-same-leaf", ERROR_NOT_SAME_OBJECT);
  evidence->null_pin_reuse = 1;
  Check(child->Stop(10000), "package-reuse-stop-new-profile-child");
  child.reset();

  const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  Require(static_cast<bool>(event), "package-reuse-wrong-type-control");
  spec.expected_package_objects = event.get();
  stage = "package-reuse.wrong-type";
  DWORD rejected = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  Require(rejected == ERROR_NOT_SAME_OBJECT && !child && stage &&
      std::strcmp(stage, "lowbox.verify-package-namespace") == 0,
      "package-reuse-wrong-type-pin-rejected-before-wrapper", ERROR_INVALID_DATA);
  evidence->wrong_type_rejected = 1;
  spec.expected_package_objects = original.app_container_objects;
  stage = "package-reuse.wrong-object";
  rejected = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  Require(rejected == ERROR_NOT_SAME_OBJECT && !child && stage &&
      std::strcmp(stage, "lowbox.verify-package-namespace") == 0,
      "package-reuse-other-directory-pin-rejected-before-wrapper", ERROR_INVALID_DATA);
  evidence->wrong_object_rejected = 1;

  // Deliberately alter only this uniquely named fixture-owned leaf under the
  // trusted SYSTEM caller. Restore it before asserting the result. This is a
  // live-policy rejection test, never a production repair/adoption operation.
  DWORD opened = 0;
  const Handle writable = OpenSessionDirectory(path, READ_CONTROL | WRITE_DAC, nullptr, &opened);
  Require(opened == 0 && writable && SameSessionObject(writable.get(), first.package_objects()),
      "package-reuse-own-fixture-policy-handle", ERROR_NOT_SAME_OBJECT);
  const Local before = SessionObjectSecurity(writable.get());
  PACL original_acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  Require(GetSecurityDescriptorDacl(before.get(), &present, &original_acl, &defaulted) && present && original_acl,
      "package-reuse-original-policy");
  EXPLICIT_ACCESSW extra{};
  extra.grfAccessPermissions = WRITE_DAC;
  extra.grfAccessMode = GRANT_ACCESS;
  extra.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  extra.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
  extra.Trustee.ptstrName = static_cast<LPWSTR>(original.capabilities.AppContainerSid);
  PACL changed_raw = nullptr;
  Check(SetEntriesInAclW(1, &extra, original_acl, &changed_raw), "package-reuse-fixture-changed-policy");
  const Local changed(changed_raw);
  Check(SetSecurityInfo(writable.get(), SE_KERNEL_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, changed_raw, nullptr),
      "package-reuse-apply-fixture-policy-change");
  PSECURITY_DESCRIPTOR applied_raw = nullptr;
  PACL applied_acl = nullptr;
  const DWORD read_changed = GetSecurityInfo(writable.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
      nullptr, nullptr, &applied_acl, nullptr, &applied_raw);
  const Local applied(applied_raw);
  const bool change_observed = read_changed == ERROR_SUCCESS && applied_acl &&
      applied_acl->AclSize == changed_raw->AclSize &&
      std::memcmp(applied_acl, changed_raw, changed_raw->AclSize) == 0 &&
      (applied_acl->AclSize != original_acl->AclSize ||
       std::memcmp(applied_acl, original_acl, original_acl->AclSize) != 0);
  spec.expected_package_objects = first.package_objects();
  stage = "package-reuse.changed-policy";
  rejected = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  const DWORD restored = SetSecurityInfo(writable.get(), SE_KERNEL_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, original_acl, nullptr);
  Check(restored, "package-reuse-restore-owned-fixture-policy");
  Require(change_observed, "package-reuse-live-policy-change-observed", ERROR_INVALID_ACL);
  const Local after = SessionObjectSecurity(writable.get());
  VerifyPackageLeafPolicy(after.get(), original);
  SECURITY_DESCRIPTOR_CONTROL before_control{}, after_control{};
  DWORD revision = 0;
  Require(GetSecurityDescriptorControl(before.get(), &before_control, &revision) &&
      GetSecurityDescriptorControl(after.get(), &after_control, &revision) && before_control == after_control,
      "package-reuse-fixture-control-restored", ERROR_INVALID_SECURITY_DESCR);
  Require(rejected == ERROR_ACCESS_DENIED && !child && stage &&
      std::strcmp(stage, "lowbox.verify-package-namespace") == 0,
      "package-reuse-pinned-but-changed-policy-rejected", ERROR_INVALID_DATA);
  evidence->changed_policy_rejected = 1;
  // Exercise a real NPFS descriptor, not just the normal object Directory.
  // Only this uniquely named fixture-owned leaf is changed; restore it before
  // asserting that a second authority launch rejected its otherwise-live name.
  DWORD pipe_status = 0;
  const Handle pipe_writable = OpenPackagePipe(original, READ_CONTROL | WRITE_DAC, nullptr, &pipe_status);
  Require(pipe_status == 0 && pipe_writable, "pipe-reuse-owned-policy-handle", ERROR_ACCESS_DENIED);
  const Local pipe_before = SessionObjectSecurity(pipe_writable.get());
  PACL pipe_acl = nullptr;
  Require(GetSecurityDescriptorDacl(pipe_before.get(), &present, &pipe_acl, &defaulted) && present && pipe_acl,
      "pipe-reuse-original-policy");
  PACL pipe_changed_raw = nullptr;
  Check(SetEntriesInAclW(1, &extra, pipe_acl, &pipe_changed_raw), "pipe-reuse-fixture-changed-policy");
  const Local pipe_changed(pipe_changed_raw);
  Check(SetSecurityInfo(pipe_writable.get(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, pipe_changed_raw, nullptr),
      "pipe-reuse-apply-fixture-policy-change");
  // Different NPFS FILE_OBJECT handles can refer to the same secure prefix.
  // Read the temporary DACL via both handles before restoring it, so the fixture
  // proves that the retained read-only pin refers to this exact changed leaf.
  PSECURITY_DESCRIPTOR pipe_applied_raw = nullptr, retained_applied_raw = nullptr;
  const DWORD pipe_applied_status = GetSecurityInfo(pipe_writable.get(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &pipe_applied_raw);
  const DWORD retained_applied_status = GetSecurityInfo(first.package_pipe_objects(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &retained_applied_raw);
  const Local pipe_applied(pipe_applied_raw), retained_applied(retained_applied_raw);
  stage = "pipe-reuse.changed-policy";
  rejected = storage::AuthorityLaunch::Create(private_token, spec, &child, &stage);
  const DWORD pipe_restored = SetSecurityInfo(pipe_writable.get(), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, pipe_acl, nullptr);
  Check(pipe_restored, "pipe-reuse-restore-owned-fixture-policy");
  const auto changed_bytes = SessionAclBytes(pipe_changed_raw);
  const auto observed_change = [&changed_bytes](DWORD read_status, PSECURITY_DESCRIPTOR security) {
    BOOL has_acl = FALSE, default_acl = FALSE;
    PACL actual = nullptr;
    return read_status == ERROR_SUCCESS && security &&
        GetSecurityDescriptorDacl(security, &has_acl, &actual, &default_acl) &&
        has_acl && actual && SessionAclBytes(actual) == changed_bytes;
  };
  Require(changed_bytes != SessionAclBytes(pipe_acl) &&
      observed_change(pipe_applied_status, pipe_applied.get()) &&
      observed_change(retained_applied_status, retained_applied.get()),
      "pipe-reuse-exact-path-and-retained-pin-share-live-policy", ERROR_INVALID_ACL);
  const Local pipe_after = SessionObjectSecurity(first.package_pipe_objects());
  VerifyPackageLeafPolicy(pipe_after.get(), original, true);
  Require(rejected == ERROR_ACCESS_DENIED && !child && stage &&
      std::strcmp(stage, "lowbox.verify-package-pipe-namespace") == 0,
      "pipe-reuse-changed-policy-rejected-before-wrapper", ERROR_INVALID_DATA);
  evidence->pipe_changed_policy_rejected = 1;
  ObservePackageSaclReuse(private_token, original, first, path, evidence);
}

Local CollisionSecurity(PSID desktop, PSID private_user, PSID package, bool changed, bool system_owner = false) {
  const std::wstring owner = SidText(desktop);
  std::wstring text = L"O:" + (system_owner ? std::wstring(L"SY") : owner) +
      L"D:P(A;;0x000f000f;;;SY)(A;;0x000f000f;;;" + owner +
      L")(A;;0x000f000f;;;" + SidText(private_user) + L")(A;;0x000f000f;;;" + SidText(package) + L")";
  if (changed) text += L"(A;;0x00020003;;;" + owner + L")";
  // These matched ordinary/private creation controls vary only Owner. Supplying
  // an explicit SACL would itself require SeSecurityPrivilege and confound the
  // owner-assignment result. Both variants use the same inherited/default label.
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1,
      &descriptor, nullptr), "collision-fixture-security");
  return Local(descriptor);
}
void ObserveNamespaceCollision(storage::ProtectedIdentity& identity, HANDLE desktop, PSID desktop_sid,
    const storage::AuthorityLaunchSpec& original, const std::wstring& name,
    container::Sid* package, NamespaceCollisionEvidence* evidence) {
  evidence->attempted = 1;
  Require(ImpersonateLoggedOnUser(identity.token()), "collision-profile-impersonate");
  const HRESULT profile = CreateAppContainerProfile(name.c_str(), name.c_str(), name.c_str(),
      nullptr, 0, &package->value);
  if (!RevertToSelf()) std::abort();
  Require(profile == S_OK && package->value, "collision-create-fresh-profile",
      FAILED(profile) ? HRESULT_CODE(profile) : ERROR_INVALID_DATA);
  evidence->profile_created = 1;
  const std::wstring path = PackageObjectPath(identity.token(), package->value, identity.session());
  CopyNamespaceText(path, evidence->path);
  RequireAbsentPackage(path, &evidence->absent);
  // Disposable object in a unique fixture-owned profile. This never opens an
  // existing object for creation, never makes it permanent and never changes a
  // production namespace leaf. Keep its exact handle until the probe ends.
  using CreateDirectory = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
  const auto create = reinterpret_cast<CreateDirectory>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateDirectoryObject")));
  Require(create != nullptr, "collision-directory-export", ERROR_PROC_NOT_FOUND);
  const Local descriptor = CollisionSecurity(desktop_sid, identity.sid(), package->value, false);
  UNICODE_STRING object_name{};
  object_name.Buffer = const_cast<wchar_t*>(path.data());
  object_name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
  object_name.MaximumLength = object_name.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.ObjectName = &object_name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE | 0x00001000;  // No OPENIF or PERMANENT.
  attributes.SecurityDescriptor = descriptor.get();
  {
    // Match the wrapper's restricted private user/groups/owner eligibility.
    // The disposable token object's own query ACL is unrelated to assigning
    // the owner of this explicitly described Directory.
    HANDLE private_raw = nullptr;
    Require(CreateRestrictedToken(identity.token(), DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr,
        0, nullptr, &private_raw), "collision-private-restricted-context");
    const Handle private_context(private_raw);
    const auto actual_user = User(private_context.get());
    Require(EqualSid(const_cast<BYTE*>(actual_user.data()), identity.sid()),
        "collision-private-actual-user", ERROR_ACCESS_DENIED);
    CopyNamespaceText(SidText(const_cast<BYTE*>(actual_user.data())), evidence->private_user);
    alignas(TOKEN_OWNER) BYTE owner_buffer[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE]{};
    DWORD bytes = 0;
    Require(GetTokenInformation(private_context.get(), TokenOwner, owner_buffer, sizeof(owner_buffer), &bytes),
        "collision-private-token-owner");
    PSID token_owner = reinterpret_cast<TOKEN_OWNER*>(owner_buffer)->Owner;
    Require(BoundedSid(owner_buffer, bytes, token_owner), "collision-private-token-owner-bound", ERROR_INVALID_SID);
    CopyNamespaceText(SidText(token_owner), evidence->private_token_owner);
    GetTokenInformation(private_context.get(), TokenGroups, nullptr, 0, &bytes);
    Require(bytes >= sizeof(TOKEN_GROUPS) && bytes <= 65536, "collision-private-groups-bound", ERROR_INVALID_DATA);
    std::vector<BYTE> group_buffer(bytes);
    Require(GetTokenInformation(private_context.get(), TokenGroups, group_buffer.data(), bytes, &bytes),
        "collision-private-token-groups");
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(group_buffer.data());
    Require(groups->GroupCount <= (bytes - offsetof(TOKEN_GROUPS, Groups)) / sizeof(SID_AND_ATTRIBUTES),
        "collision-private-group-count", ERROR_INVALID_DATA);
    evidence->private_group_count = groups->GroupCount;
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
      const auto& group = groups->Groups[i];
      Require(BoundedSid(group_buffer.data(), bytes, group.Sid), "collision-private-group-sid", ERROR_INVALID_SID);
      if (IsWellKnownSid(group.Sid, WinLocalSystemSid)) evidence->private_system_group_attributes |= group.Attributes;
    }
    const Local private_control = CollisionSecurity(identity.sid(), identity.sid(), package->value, false);
    const Local private_forgery = CollisionSecurity(identity.sid(), identity.sid(), package->value, false, true);
    for (unsigned phase = 0; phase < 3; ++phase) {
      {
        attributes.SecurityDescriptor = phase == 1 ? private_forgery.get() : private_control.get();
        Require(ImpersonateLoggedOnUser(private_context.get()), "collision-private-owner-impersonate");
        HANDLE probe_raw = nullptr;
        const NTSTATUS probe_status = create(&probe_raw, 0x000f000f, &attributes);
        if (!RevertToSelf()) std::abort();
        const Handle probe(probe_raw);
        if (phase == 1) {
          evidence->private_owner_status = static_cast<DWORD>(probe_status);
          Require(!probe && (probe_status == static_cast<NTSTATUS>(0xc000005a) ||
              probe_status == static_cast<NTSTATUS>(0xc0000061)),
              "collision-private-system-owner-assignment-rejected", ERROR_INVALID_OWNER);
        } else {
          Require(probe_status == 0 && probe, "collision-private-owner-positive-create", ERROR_ACCESS_DENIED);
          const Local observed = SessionObjectSecurity(probe.get());
          PSID owner = nullptr;
          BOOL defaulted = FALSE;
          Require(GetSecurityDescriptorOwner(observed.get(), &owner, &defaulted) && owner &&
              EqualSid(owner, identity.sid()), "collision-private-owner-positive-identity", ERROR_INVALID_OWNER);
          ++evidence->private_owner_controls;
        }
      }
      RequireAbsentPackage(path, &evidence->absent);
    }
    attributes.SecurityDescriptor = descriptor.get();
  }
  {
    Require(ImpersonateLoggedOnUser(desktop), "collision-owner-control-impersonate");
    HANDLE raw_control = nullptr;
    const NTSTATUS control_status = create(&raw_control, 0x000f000f, &attributes);
    if (!RevertToSelf()) std::abort();
    const Handle control(raw_control);
    Require(control_status == 0 && control, "collision-owner-control-actual-create", ERROR_ACCESS_DENIED);
    const Local control_security = SessionObjectSecurity(control.get());
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    Require(GetSecurityDescriptorOwner(control_security.get(), &owner, &defaulted) && owner &&
        EqualSid(owner, desktop_sid), "collision-owner-control-actual-desktop-owner", ERROR_INVALID_OWNER);
    evidence->system_owner_control_created = 1;
  }
  RequireAbsentPackage(path, &evidence->absent);
  // Same parent, requested mask, name and allow ACEs as the positive desktop
  // creation below, but with SYSTEM explicitly requested as owner. Require an
  // attributable owner-assignment rejection, not merely a generic access error.
  // This establishes why a desktop-owned object cannot become an accepted
  // SYSTEM-owned leaf while its original mutation handles remain open.
  const Local forged = CollisionSecurity(desktop_sid, identity.sid(), package->value, false, true);
  attributes.SecurityDescriptor = forged.get();
  evidence->system_owner_attempted = 1;
  Require(ImpersonateLoggedOnUser(desktop), "collision-desktop-forged-owner-impersonate");
  HANDLE forbidden = nullptr;
  const NTSTATUS forged_status = create(&forbidden, 0x000f000f, &attributes);
  if (!RevertToSelf()) std::abort();
  const Handle forbidden_owner(forbidden);
  evidence->system_owner_status = static_cast<DWORD>(forged_status);
  Require(!forbidden_owner && (forged_status == static_cast<NTSTATUS>(0xc000005a) ||
      forged_status == static_cast<NTSTATUS>(0xc0000061)),
      "collision-desktop-system-owner-assignment-rejected", ERROR_INVALID_OWNER);
  RequireAbsentPackage(path, &evidence->absent);
  attributes.SecurityDescriptor = descriptor.get();
  Require(ImpersonateLoggedOnUser(desktop), "collision-desktop-create-impersonate");
  HANDLE raw = nullptr;
  const NTSTATUS created = create(&raw, 0x000f000f, &attributes);
  if (!RevertToSelf()) std::abort();
  const Handle pinned(raw);
  evidence->create_status = static_cast<DWORD>(created);
  Require(created == 0 && pinned &&
      SessionObjectText(pinned.get(), ObjectTypeInformation) == L"Directory" &&
      SessionObjectText(pinned.get(), static_cast<OBJECT_INFORMATION_CLASS>(1)) == path,
      "collision-fresh-desktop-directory", ERROR_INVALID_DATA);
  const Local initial = SessionObjectSecurity(pinned.get());
  PSID owner = nullptr;
  BOOL defaulted = FALSE;
  Require(GetSecurityDescriptorOwner(initial.get(), &owner, &defaulted) && owner &&
      EqualSid(owner, desktop_sid), "collision-desktop-owner", ERROR_INVALID_OWNER);
  evidence->desktop_owner = 1;
  DWORD status = 0;
  const Handle writable = OpenSessionDirectory(path, WRITE_DAC, desktop, &status);
  Require(status == 0 && writable && SameSessionObject(pinned.get(), writable.get()),
      "collision-desktop-writable-handle", ERROR_ACCESS_DENIED);
  const Local changed = CollisionSecurity(desktop_sid, identity.sid(), package->value, true);
  PACL acl = nullptr;
  BOOL present = FALSE;
  Require(GetSecurityDescriptorDacl(changed.get(), &present, &acl, &defaulted) && present && acl,
      "collision-fixture-dacl");
  Require(ImpersonateLoggedOnUser(desktop), "collision-desktop-dacl-impersonate");
  const DWORD changed_status = SetSecurityInfo(writable.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
      nullptr, nullptr, acl, nullptr);
  if (!RevertToSelf()) std::abort();
  Check(changed_status, "collision-desktop-dacl-write");
  const Local observed = SessionObjectSecurity(pinned.get());
  PACL actual_acl = nullptr, initial_acl = nullptr;
  Require(GetSecurityDescriptorDacl(observed.get(), &present, &actual_acl, &defaulted) && present && actual_acl &&
      GetSecurityDescriptorDacl(initial.get(), &present, &initial_acl, &defaulted) && present && initial_acl &&
      SessionAclBytes(actual_acl) == SessionAclBytes(acl) &&
      SessionAclBytes(actual_acl) != SessionAclBytes(initial_acl),
      "collision-desktop-dacl-write-observed", ERROR_INVALID_ACL);
  evidence->desktop_dacl_changed = 1;
  CopyObjectSecurity(observed.get(), evidence->security);
  const Handle private_open = OpenSessionDirectory(path, 0x0000000f, identity.token(), &evidence->private_open_status);
  Require(evidence->private_open_status == 0 && private_open && SameSessionObject(pinned.get(), private_open.get()),
      "collision-private-access-positive-control", ERROR_ACCESS_DENIED);
  auto spec = original;
  spec.capabilities.AppContainerSid = package->value;
  storage::AuthorityNamespaceCreateResult namespace_result;
  spec.namespace_create_result = &namespace_result;
  std::unique_ptr<storage::AuthorityLaunch> child;
  const char* stage = "collision.launch";
  evidence->launch_called = 1;
  evidence->launch_status = storage::AuthorityLaunch::Create(identity.token(), spec, &child, &stage);
  std::snprintf(evidence->stage, sizeof(evidence->stage), "%s", stage ? stage : "<null>");
  evidence->returned_child = static_cast<DWORD>(static_cast<bool>(child));
  if (child) {
    // This adversarial child must never Resume or receive application resources.
    evidence->stopped_child = child->Stop(10000);
    child.reset();
    Check(evidence->stopped_child, "collision-stop-suspended-child");
  }
  const Handle fresh = OpenSessionDirectory(path, 0x00020003, nullptr, &status);
  Require(status == 0 && fresh && SameSessionObject(pinned.get(), fresh.get()),
      "collision-retained-exact-object", ERROR_NOT_SAME_OBJECT);
  evidence->same_object_after = 1;
  PACL label = nullptr;
  Require(GetSecurityDescriptorSacl(observed.get(), &present, &label, &defaulted),
      "collision-observed-label-presence", ERROR_INVALID_SECURITY_DESCR);
  evidence->explicit_label = present && label;
  using PolicyStage = storage::AuthorityNamespacePolicyStage;
  evidence->policy_stage = static_cast<DWORD>(namespace_result.directory_policy.stage);
  Require(evidence->launch_status == (evidence->explicit_label ? ERROR_ACCESS_DENIED : ERROR_INVALID_SECURITY_DESCR) &&
      namespace_result.directory_policy.stage == (evidence->explicit_label ? PolicyStage::kPolicy : PolicyStage::kLabel) &&
      !evidence->returned_child && stage && std::strcmp(stage, "lowbox.verify-package-namespace") == 0,
      "deny-desktop-owned-namespace-collision", ERROR_ACCESS_DENIED);

  // A complete descriptor must still be rejected at the policy check. Give
  // this disposable desktop-owned control an explicit label under SYSTEM,
  // and leave its owner and DACL intact. Last close removes the temporary leaf.
  PSECURITY_DESCRIPTOR labeled_raw = nullptr;
  Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:P(ML;;NW;;;LW)",
      SDDL_REVISION_1, &labeled_raw, nullptr), "collision-complete-label-descriptor");
  const Local labeled(labeled_raw);
  const Handle label_writer = OpenFixtureSacl(path);
  Require(SameSessionObject(pinned.get(), label_writer.get()),
      "collision-label-control-exact-object", ERROR_NOT_SAME_OBJECT);
  Require(SecuritySetter()(label_writer.get(), SACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION,
      labeled.get()) == 0, "collision-complete-label-write", ERROR_ACCESS_DENIED);
  const Local complete = SessionObjectSecurity(pinned.get());
  PACL present_label = nullptr, unchanged_acl = nullptr;
  Require(GetSecurityDescriptorOwner(complete.get(), &owner, &defaulted) && owner && EqualSid(owner, desktop_sid) &&
      GetSecurityDescriptorSacl(complete.get(), &present, &present_label, &defaulted) && present && present_label &&
      GetSecurityDescriptorDacl(complete.get(), &present, &unchanged_acl, &defaulted) && present && unchanged_acl &&
      SessionAclBytes(unchanged_acl) == SessionAclBytes(actual_acl),
      "collision-complete-label-and-unchanged-desktop-policy", ERROR_INVALID_SECURITY_DESCR);
  namespace_result = {};
  stage = "collision.labeled-launch";
  evidence->labeled_launch_status = storage::AuthorityLaunch::Create(identity.token(), spec, &child, &stage);
  evidence->labeled_policy_stage = static_cast<DWORD>(namespace_result.directory_policy.stage);
  if (child) {
    evidence->stopped_child = child->Stop(10000);
    child.reset();
    Check(evidence->stopped_child, "collision-stop-labeled-suspended-child");
    Require(false, "deny-labeled-desktop-owned-namespace-collision", ERROR_ACCESS_DENIED);
  }
  const Handle after = OpenSessionDirectory(path, 0x00020003, nullptr, &status);
  Require(evidence->labeled_launch_status == ERROR_ACCESS_DENIED &&
      namespace_result.directory_policy.stage == PolicyStage::kPolicy &&
      stage && std::strcmp(stage, "lowbox.verify-package-namespace") == 0 &&
      status == 0 && after && SameSessionObject(pinned.get(), after.get()),
      "deny-labeled-desktop-owned-namespace-collision", ERROR_ACCESS_DENIED);
  evidence->status = ERROR_SUCCESS;
}
void PrintPolicyReadback(const char* object, const storage::AuthorityNamespacePolicyResult& observed) {
  using Stage = storage::AuthorityNamespacePolicyStage;
  const char* stage = "invalid";
  switch (observed.stage) {
    case Stage::kNotAttempted: stage = "not-attempted"; break;
    case Stage::kSizeQuery: stage = "native-size-query"; break;
    case Stage::kQuery: stage = "native-query"; break;
    case Stage::kLength: stage = "returned-length"; break;
    case Stage::kControl: stage = "relative-control"; break;
    case Stage::kOwner: stage = "owner-getter"; break;
    case Stage::kGroup: stage = "group-getter"; break;
    case Stage::kDacl: stage = "dacl-getter-present"; break;
    case Stage::kLabel: stage = "label-getter-present"; break;
    case Stage::kOwnerBounds: stage = "owner-bounds"; break;
    case Stage::kGroupBounds: stage = "group-bounds"; break;
    case Stage::kDaclBounds: stage = "dacl-bounds"; break;
    case Stage::kLabelBounds: stage = "label-bounds"; break;
    case Stage::kDescriptor: stage = "valid-descriptor"; break;
    case Stage::kPolicy: stage = "exact-policy"; break;
    case Stage::kComplete: stage = "complete"; break;
  }
  Require(std::strcmp(stage, "invalid") != 0 && observed.captured_bytes <= sizeof(observed.descriptor) &&
      observed.captured_bytes <= observed.allocation_bytes,
      "bounded-namespace-policy-readback", ERROR_INVALID_DATA);
  std::printf("Protected %s policy readback: stage=%s size-ntstatus=0x%08lx allocation-bytes=%lu query-ntstatus=0x%08lx requested=0x%08lx returned-bytes=%lu allocation-prefix-bytes=%lu\n",
      object, stage, static_cast<unsigned long>(observed.size_query_status), static_cast<unsigned long>(observed.allocation_bytes),
      static_cast<unsigned long>(observed.query_status), static_cast<unsigned long>(observed.requested_information),
      static_cast<unsigned long>(observed.returned_bytes),
      static_cast<unsigned long>(observed.captured_bytes));
  if (observed.captured_bytes >= sizeof(SECURITY_DESCRIPTOR_RELATIVE)) {
    SECURITY_DESCRIPTOR_RELATIVE header{};
    std::memcpy(&header, observed.descriptor, sizeof(header));
    std::printf("Protected %s policy allocation relative header: revision=%u control=0x%04x owner-offset=%lu group-offset=%lu dacl-offset=%lu sacl-offset=%lu\n",
        object, static_cast<unsigned int>(header.Revision), static_cast<unsigned int>(header.Control),
        static_cast<unsigned long>(header.Owner), static_cast<unsigned long>(header.Group),
        static_cast<unsigned long>(header.Dacl), static_cast<unsigned long>(header.Sacl));
  }
  // Print bytes without dereferencing any offsets from a rejected descriptor.
  // This is an allocation prefix, including any unwritten zero-filled bytes.
  // It is never substituted for the reported length in policy verification.
  std::printf("Protected %s policy allocation prefix hex: ", object);
  for (DWORD i = 0; i < observed.captured_bytes; ++i) {
    std::printf("%02x", static_cast<unsigned int>(observed.descriptor[i]));
  }
  std::puts("");
}

void PrintNamespaceRecheck(const storage::AuthorityNamespaceRecheckResult& observed) {
  using Stage = storage::AuthorityNamespaceRecheckStage;
  const char* stage = "invalid";
  switch (observed.stage) {
    case Stage::kNotAttempted: stage = "not-attempted"; break;
    case Stage::kLeafOpen: stage = "leaf-open"; break;
    case Stage::kSamePin: stage = "retained-object-identity"; break;
    case Stage::kDirectoryPolicy: stage = "directory-policy"; break;
    case Stage::kPipePolicy: stage = "pipe-policy"; break;
    case Stage::kComplete: stage = "complete"; break;
  }
  Require(std::strcmp(stage, "invalid") != 0, "bounded-namespace-recheck-stage", ERROR_INVALID_DATA);
  std::printf("Protected package recheck: stage=%s attempts=%lu win32=%lu leaf-open-ntstatus=0x%08lx\n",
      stage, static_cast<unsigned long>(observed.attempts), static_cast<unsigned long>(observed.status),
      static_cast<unsigned long>(observed.leaf_open_status));
  PrintPolicyReadback("recheck package", observed.directory_policy);
  PrintPolicyReadback("recheck pipe", observed.pipe_policy);
}

void PrintNamespaceSeal(const storage::AuthorityNamespaceSealResult& observed) {
  using Stage = storage::AuthorityNamespaceSealStage;
  const char* stage = "invalid";
  switch (observed.stage) {
    case Stage::kNotAttempted: stage = "not-attempted"; break;
    case Stage::kPrepared: stage = "prepared"; break;
    case Stage::kPrivilege: stage = "privilege"; break;
    case Stage::kBind: stage = "bind"; break;
    case Stage::kOpen: stage = "open"; break;
    case Stage::kSamePin: stage = "retained-object-identity"; break;
    case Stage::kFullPolicy: stage = "full-policy"; break;
    case Stage::kDescriptor: stage = "descriptor"; break;
    case Stage::kWrite: stage = "write"; break;
    case Stage::kReadback: stage = "readback"; break;
    case Stage::kClose: stage = "close"; break;
    case Stage::kRestore: stage = "restore"; break;
    case Stage::kComplete: stage = "complete"; break;
  }
  Require(std::strcmp(stage, "invalid") != 0 && observed.write_called <= 1 && observed.close_called <= 1,
      "bounded-namespace-seal-stage", ERROR_INVALID_DATA);
  std::printf("Protected package SACL reseal: stage=%s win32=%lu open-ntstatus=0x%08lx write-called=%lu write-ntstatus=0x%08lx close-called=%lu close-win32=%lu\n",
      stage, static_cast<unsigned long>(observed.status), static_cast<unsigned long>(observed.open_status),
      static_cast<unsigned long>(observed.write_called), static_cast<unsigned long>(observed.write_status),
      static_cast<unsigned long>(observed.close_called), static_cast<unsigned long>(observed.close_status));
  std::printf("Protected package SACL reseal context: privilege-enable-win32=%lu bind-win32=%lu before=0x%08lx enabled=0x%08lx restored=0x%08lx changed-count=%lu privilege-restore-win32=%lu context-restore-win32=%lu\n",
      static_cast<unsigned long>(observed.privilege_status), static_cast<unsigned long>(observed.bind_status),
      static_cast<unsigned long>(observed.privilege_before), static_cast<unsigned long>(observed.privilege_enabled),
      static_cast<unsigned long>(observed.privilege_restored), static_cast<unsigned long>(observed.privilege_changes),
      static_cast<unsigned long>(observed.privilege_restore_status), static_cast<unsigned long>(observed.context_restore_status));
  PrintPolicyReadback("reseal full-before", observed.before_policy);
  PrintPolicyReadback("reseal full-after", observed.after_policy);
}

void PrintPackageNamespace(const PackageLeafEvidence& leaf, const NamespaceCollisionEvidence& collision) {
  std::printf("Protected package leaf: absent-before-launch=%lu captured=%lu status=%lu control=0x%04lx path=%ls\n",
      static_cast<unsigned long>(leaf.absent_before_launch), static_cast<unsigned long>(leaf.captured),
      static_cast<unsigned long>(leaf.status), static_cast<unsigned long>(leaf.control), NamespacePathForPrint(leaf.path));
  if (leaf.captured && std::wmemchr(leaf.security, L'\0', std::size(leaf.security)))
    std::printf("Protected package leaf: owner-group-dacl-label=%ls\n", leaf.security);
  const auto& event = leaf.event_creation;
  if (event.called)
    std::printf("Protected child event: status=0x%08lx handle=%lu signal=%lu signal-error=%lu "
        "wait=0x%08lx wait-error=%lu type-match=%lu name-match=%lu access=0x%08lx expected=0x%08lx\n",
        static_cast<unsigned long>(event.status), static_cast<unsigned long>(event.opened),
        static_cast<unsigned long>(event.signaled), static_cast<unsigned long>(event.signal_error),
        static_cast<unsigned long>(event.waited), static_cast<unsigned long>(event.wait_error),
        static_cast<unsigned long>(event.type_matches), static_cast<unsigned long>(event.name_matches),
        static_cast<unsigned long>(event.granted_access), static_cast<unsigned long>(kChildEventUseAccess));
  std::printf("Protected package admission: exact-policy=%lu launch-readonly-pin=%lu actual-event=%lu actual-subdirectory=%lu expected-pin-reuse=%lu null-pin-reuse=%lu independent-pin-lifetime=%lu wrong-type-rejected=%lu wrong-object-rejected=%lu changed-policy-rejected=%lu\n",
      static_cast<unsigned long>(leaf.policy_verified), static_cast<unsigned long>(leaf.launch_pin_verified),
      static_cast<unsigned long>(leaf.object_created), static_cast<unsigned long>(leaf.subdirectory_created),
      static_cast<unsigned long>(leaf.expected_reuse), static_cast<unsigned long>(leaf.null_pin_reuse),
      static_cast<unsigned long>(leaf.independent_pin_lifetime), static_cast<unsigned long>(leaf.wrong_type_rejected),
      static_cast<unsigned long>(leaf.wrong_object_rejected), static_cast<unsigned long>(leaf.changed_policy_rejected));
  std::printf("Protected pipe admission: launch-readonly-pin=%lu exact-policy=%lu actual-package-read=%lu mutation-and-other-read-denials=%lu independent-reuse-pin=%lu changed-policy-rejected=%lu\n",
      static_cast<unsigned long>(leaf.pipe_pin_verified), static_cast<unsigned long>(leaf.pipe_policy_verified),
      static_cast<unsigned long>(leaf.pipe_positive), static_cast<unsigned long>(leaf.pipe_denials),
      static_cast<unsigned long>(leaf.pipe_reused), static_cast<unsigned long>(leaf.pipe_changed_policy_rejected));
  const auto& pipe = leaf.pipe_pin;
  if (pipe.called)
    std::printf("Protected pipe pin: handle=%lu info-ok=%lu info-win32=%lu flags=0x%08lx access=0x%08lx "
        "expected=0x%08lx type-match=%lu device-name-match=%lu name=%ls\n",
        static_cast<unsigned long>(pipe.present), static_cast<unsigned long>(pipe.info_ok),
        static_cast<unsigned long>(pipe.info_status),
        static_cast<unsigned long>(pipe.flags), static_cast<unsigned long>(pipe.granted_access),
        static_cast<unsigned long>(READ_CONTROL | SYNCHRONIZE), static_cast<unsigned long>(pipe.type_matches),
        static_cast<unsigned long>(pipe.device_name_matches), NamespacePathForPrint(pipe.name));
  std::printf("Protected SACL controls: mutation-denials=%lu preexisting-unprotected-rejected=%lu extra-audit-rejected=%lu audit-preserved=%lu fixture-restored=%lu\n",
      static_cast<unsigned long>(leaf.sacl_mutation_denials), static_cast<unsigned long>(leaf.unprotected_reuse_rejected),
      static_cast<unsigned long>(leaf.audit_reuse_rejected), static_cast<unsigned long>(leaf.audit_preserved),
      static_cast<unsigned long>(leaf.sacl_fixture_restored));
  std::printf("Protected policy admission: own-read=%lu other-package-denials=%lu modification-denials=%lu atomic-replacement-read=%lu path-override-denied=%lu\n",
      static_cast<unsigned long>(leaf.policy_own_read), static_cast<unsigned long>(leaf.policy_other_denied),
      static_cast<unsigned long>(leaf.policy_write_denials), static_cast<unsigned long>(leaf.policy_replacement),
      static_cast<unsigned long>(leaf.policy_bad_path_denied));
  if (leaf.event_security[0] && std::wmemchr(leaf.event_security, L'\0', std::size(leaf.event_security)))
    std::printf("Protected package child: kind=event inherited-owner-group-dacl-label=%ls\n", leaf.event_security);
  if (leaf.directory_security[0] && std::wmemchr(leaf.directory_security, L'\0', std::size(leaf.directory_security)))
    std::printf("Protected package child: kind=directory inherited-owner-group-dacl-label=%ls\n", leaf.directory_security);
  constexpr const char* child_subjects[] = {"actual-package", "other-package", "filtered-desktop"};
  for (size_t kind = 0; kind < 2; ++kind) {
    for (size_t actor = 0; actor < 3; ++actor) {
      const auto& row = leaf.child_opens[kind][actor];
      std::printf("Protected package child reopen: kind=%s subject=%s called=%lu ntstatus=0x%08lx opened=%lu own-use-after-reopen=%lu individual-operation-and-mutation-denials=%lu\n",
          kind == 0 ? "event" : "directory", child_subjects[actor], static_cast<unsigned long>(row.called),
          static_cast<unsigned long>(row.status), static_cast<unsigned long>(row.opened),
          static_cast<unsigned long>(actor == 0 ? leaf.child_used[kind] : 0),
          static_cast<unsigned long>(actor == 0 ? 0 : leaf.child_denials[kind][actor - 1]));
    }
  }
  constexpr const char* subjects[] = {"own-suspended-lowbox", "distinct-package-token-only", "filtered-desktop"};
  for (size_t actor = 0; actor < std::size(subjects); ++actor) {
    for (size_t i = 0; i < std::size(kSessionMutationMasks); ++i) {
      const auto& row = leaf.opens[actor][i];
      std::printf("Protected package leaf: subject=%s requested=0x%08lx called=%lu ntstatus=0x%08lx opened=%lu\n",
          subjects[actor], static_cast<unsigned long>(kSessionMutationMasks[i]), static_cast<unsigned long>(row.called),
          static_cast<unsigned long>(row.status), static_cast<unsigned long>(row.opened));
    }
  }
  std::printf("Protected namespace collision: attempted=%lu status=%lu profile-created=%lu absent=%lu create-ntstatus=0x%08lx desktop-owner=%lu desktop-dacl-changed=%lu private-open-ntstatus=0x%08lx path=%ls\n",
      static_cast<unsigned long>(collision.attempted), static_cast<unsigned long>(collision.status),
      static_cast<unsigned long>(collision.profile_created), static_cast<unsigned long>(collision.absent),
      static_cast<unsigned long>(collision.create_status), static_cast<unsigned long>(collision.desktop_owner),
      static_cast<unsigned long>(collision.desktop_dacl_changed), static_cast<unsigned long>(collision.private_open_status),
      NamespacePathForPrint(collision.path));
  std::printf("Protected namespace owner assignment: desktop-create-control=%lu system-owner-attempted=%lu system-owner-ntstatus=0x%08lx trust-limit=SYSTEM-and-administrators-able-to-act-as-SYSTEM\n",
      static_cast<unsigned long>(collision.system_owner_control_created),
      static_cast<unsigned long>(collision.system_owner_attempted), static_cast<unsigned long>(collision.system_owner_status));
  if (std::wmemchr(collision.private_user, L'\0', std::size(collision.private_user)) &&
      std::wmemchr(collision.private_token_owner, L'\0', std::size(collision.private_token_owner))) {
    std::printf("Protected namespace private owner assignment: context=DISABLE_MAX_PRIVILEGE-nonlowbox user=%ls token-owner=%ls groups=%lu system-group-attributes=0x%08lx positive-creations=%lu system-owner-ntstatus=0x%08lx\n",
        collision.private_user, collision.private_token_owner, static_cast<unsigned long>(collision.private_group_count),
        static_cast<unsigned long>(collision.private_system_group_attributes),
        static_cast<unsigned long>(collision.private_owner_controls), static_cast<unsigned long>(collision.private_owner_status));
  }
  if (collision.desktop_dacl_changed && std::wmemchr(collision.security, L'\0', std::size(collision.security)))
    std::printf("Protected namespace collision: owner-group-dacl-label=%ls\n", collision.security);
  std::printf("Protected namespace collision: launch-called=%lu launch-win32=%lu stage=%.*s returned-child=%lu stop-win32=%lu retained-same-object=%lu resumed=0\n",
      static_cast<unsigned long>(collision.launch_called), static_cast<unsigned long>(collision.launch_status),
      static_cast<int>(sizeof(collision.stage)), collision.stage, static_cast<unsigned long>(collision.returned_child),
      static_cast<unsigned long>(collision.stopped_child), static_cast<unsigned long>(collision.same_object_after));
  std::printf("Protected namespace collision policy: explicit-label=%lu native-policy-stage=%lu labeled-launch-win32=%lu labeled-policy-stage=%lu\n",
      static_cast<unsigned long>(collision.explicit_label), static_cast<unsigned long>(collision.policy_stage),
      static_cast<unsigned long>(collision.labeled_launch_status), static_cast<unsigned long>(collision.labeled_policy_stage));
}
void PrintSessionObjects(const char* root, DWORD private_grant, const SessionObjectsEvidence& evidence) {
  std::printf("Protected session objects: root=%s private-grant=0x%08lx\n", root, static_cast<unsigned long>(private_grant));
  std::printf("Protected session objects: captured=%lu same-object=%lu granted=0x%08lx exact-owner-group-label-control-ace-delta=%lu private-open-called=%lu private-open-ntstatus=0x%08lx wrong-type-rejected=%lu wrong-session-root-rejected=%lu\n",
      static_cast<unsigned long>(evidence.captured), static_cast<unsigned long>(evidence.same_object),
      static_cast<unsigned long>(evidence.granted_access), static_cast<unsigned long>(evidence.exact_sd_delta),
      static_cast<unsigned long>(evidence.private_open_called), static_cast<unsigned long>(evidence.private_open_status),
      static_cast<unsigned long>(evidence.wrong_type_rejected), static_cast<unsigned long>(evidence.wrong_session_rejected));
  constexpr const char* subjects[] = {"actual-private", "filtered-desktop", "actual-suspended-lowbox", "alternate-package-token-boundary-only-no-wrapper-or-guest"};
  std::puts("Protected session objects: baseline-and-current AccessCheck are DACL checks; real opens use the actual subject token; these root access probes create/delete no objects and do not change security");
  for (size_t actor = 0; actor < std::size(subjects); ++actor) {
    const auto& subject = evidence.subjects[actor];
    Require(subject.attempted <= 1 && subject.checked <= std::size(kSessionMutationMasks),
        "session-objects-evidence-bound", ERROR_INVALID_DATA);
    std::printf("Protected session objects: subject=%s attempted=%lu checked=%lu\n", subjects[actor],
        static_cast<unsigned long>(subject.attempted), static_cast<unsigned long>(subject.checked));
    for (DWORD i = 0; subject.attempted && i < std::size(kSessionMutationMasks); ++i) {
      const auto& mutation = subject.mutations[i];
      Require(mutation.real_opened <= 1, "session-objects-open-evidence-bound", ERROR_INVALID_DATA);
      std::printf("Protected session objects: subject=%s mask=0x%08lx completed=%lu before=0x%08lx after=0x%08lx real-open-ntstatus=0x%08lx opened=%lu\n",
          subjects[actor], static_cast<unsigned long>(kSessionMutationMasks[i]), static_cast<unsigned long>(i < subject.checked),
          static_cast<unsigned long>(mutation.before),
          static_cast<unsigned long>(mutation.after), static_cast<unsigned long>(mutation.real_open_status),
          static_cast<unsigned long>(mutation.real_opened));
    }
  }
}
Evidence RunSystem() {
  Evidence evidence;
  bool installed = false;
  bool production_attempted = false;
  storage::ProtectedIdentityCleanup cleanup;
  std::unique_ptr<storage::ProtectedIdentity> identity;
  std::unique_ptr<storage::AuthorityLaunch> child;
  SessionObjectsSnapshot session_objects, app_container_objects;
  container::Sid package, capability, collision_package;
  std::wstring package_name, collision_package_name;
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
    session_objects = CaptureSessionObjects(config.session, L"BaseNamedObjects", &evidence.session_objects);
    app_container_objects = CaptureSessionObjects(config.session, L"AppContainerNamedObjects", &evidence.app_container_objects);
    const DWORD identity_status = storage::ProtectedIdentity::Create(config.image, config.session, &identity, &stage, &cleanup);
    Check(identity_status, stage);
    evidence.profile_identity_created = 1;
    Require(EqualSid(identity->sid(), sid.data()), "actual-private-token-user", ERROR_ACCESS_DENIED);
    VerifySessionLease(&session_objects, *identity, identity->session_objects(), 0x00020003, &evidence.session_objects);
    VerifySessionLease(&app_container_objects, *identity, identity->app_container_objects(), 0x0000000f, &evidence.app_container_objects);
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
    VerifySessionSubject(session_objects, filtered.get(), config.session, &evidence.session_objects.subjects[1]);
    VerifySessionSubject(app_container_objects, filtered.get(), config.session, &evidence.app_container_objects.subjects[1]);
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
        environment, {package.value, caps, 1, 0}, identity->sid(), config.session, &cleanup.profile_error,
        service_job.get(), identity->session_objects(), identity->app_container_objects()};
    std::wstring shadowed_environment = environment;
    shadowed_environment.pop_back();
    shadowed_environment.append(L"systemroot=C:\\untrusted").append(2, L'\0');
    storage::AuthorityLaunchSpec shadowed = spec;
    shadowed.environment = shadowed_environment;
    Require(storage::AuthorityLaunch::Create(identity->token(), shadowed, &child) == ERROR_INVALID_PARAMETER && !child,
        "deny-case-insensitive-environment-shadow", ERROR_INVALID_DATA);
    const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Require(static_cast<bool>(event), "session-objects-wrong-type-event");
    storage::AuthorityLaunchSpec wrong = spec;
    wrong.session_objects = event.get();
    DWORD rejected = storage::AuthorityLaunch::Create(identity->token(), wrong, &child, &stage);
    Require(rejected == ERROR_NOT_SAME_OBJECT && !child && stage &&
        std::strcmp(stage, "lowbox.verify-session-namespace") == 0,
        "deny-session-objects-wrong-type-before-wrapper", ERROR_INVALID_DATA);
    evidence.session_objects.wrong_type_rejected = 1;
    DWORD worker_session = 0, other_status = 0;
    Require(ProcessIdToSessionId(GetCurrentProcessId(), &worker_session) && worker_session != config.session,
        "session-objects-distinct-worker-session", ERROR_INVALID_DATA);
    // The prior actual namespace probe established this worker-session root.
    // This is a wrong-session/root Directory, not a claim that session0 has
    // an unaliased BaseNamedObjects leaf with the selected-session spelling.
    const std::wstring other_path = L"\\Sessions\\" + std::to_wstring(worker_session) + L"\\AppContainerNamedObjects";
    const Handle other_session = OpenSessionDirectory(other_path, 0x00020003, nullptr, &other_status);
    DWORD other_flags = 0;
    Require(other_status == 0 && other_session &&
        GetHandleInformation(other_session.get(), &other_flags) && !(other_flags & HANDLE_FLAG_INHERIT) &&
        SessionObjectText(other_session.get(), ObjectTypeInformation) == L"Directory" &&
        SessionObjectText(other_session.get(), static_cast<OBJECT_INFORMATION_CLASS>(1)) == other_path &&
        SessionObjectAccess(other_session.get()) == 0x00020003 &&
        !SameSessionObject(session_objects.object.get(), other_session.get()),
        "session-objects-other-session-root-control", ERROR_NOT_SAME_OBJECT);
    wrong.session_objects = other_session.get();
    rejected = storage::AuthorityLaunch::Create(identity->token(), wrong, &child, &stage);
    Require(rejected == ERROR_NOT_SAME_OBJECT && !child && stage &&
        std::strcmp(stage, "lowbox.verify-session-namespace") == 0,
        "deny-session-objects-wrong-session-before-wrapper", ERROR_INVALID_DATA);
    evidence.session_objects.wrong_session_rejected = 1;
    wrong = spec;
    wrong.app_container_objects = event.get();
    rejected = storage::AuthorityLaunch::Create(identity->token(), wrong, &child, &stage);
    Require(rejected == ERROR_NOT_SAME_OBJECT && !child && stage &&
        std::strcmp(stage, "lowbox.verify-appcontainer-namespace") == 0,
        "deny-appcontainer-objects-wrong-type-before-wrapper", ERROR_INVALID_DATA);
    evidence.app_container_objects.wrong_type_rejected = 1;
    wrong.app_container_objects = other_session.get();
    rejected = storage::AuthorityLaunch::Create(identity->token(), wrong, &child, &stage);
    Require(rejected == ERROR_NOT_SAME_OBJECT && !child && stage &&
        std::strcmp(stage, "lowbox.verify-appcontainer-namespace") == 0,
        "deny-appcontainer-objects-wrong-session-before-wrapper", ERROR_INVALID_DATA);
    evidence.app_container_objects.wrong_session_rejected = 1;
    const std::wstring package_object_path = PackageObjectPath(identity->token(), package.value, config.session);
    CopyNamespaceText(package_object_path, evidence.package_leaf.path);
    RequireAbsentPackage(package_object_path, &evidence.package_leaf.absent_before_launch);
    ObserveLaunchAccess(identity->token(), &evidence);
    HANDLE authority_token_raw = nullptr;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &authority_token_raw),
        "authority-privilege-snapshot-open");
    const Handle authority_token(authority_token_raw);
    const auto process_privileges_before = CaptureAuthorityPrivilegeBytes(authority_token.get());
    storage::AuthorityLaunchSpec observed_spec = spec;
    observed_spec.namespace_create_result = &evidence.namespace_create;
    production_attempted = true;
    ScopedLowboxTrace observer(config.lowbox_iat_trace, config.session, &evidence);
    const DWORD created = storage::AuthorityLaunch::Create(identity->token(), observed_spec, &child, &stage);
    const DWORD restored = observer.End();
    // Read actual Windows state even when launch failed, and retain the launch
    // failure as primary. This does not inject or change a production token.
    try {
      const auto process_privileges_after = CaptureAuthorityPrivilegeBytes(authority_token.get());
      evidence.namespace_process_privileges_unchanged = process_privileges_before == process_privileges_after ? 1 : 0;
      HANDLE remaining_raw = nullptr;
      const BOOL remaining = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &remaining_raw);
      const DWORD context_error = GetLastError();
      const Handle remaining_token(remaining_raw);
      evidence.namespace_no_thread_token = !remaining && context_error == ERROR_NO_TOKEN ? 1 : 0;
      if (remaining && !RevertToSelf()) std::abort();
      Require(evidence.namespace_process_privileges_unchanged && evidence.namespace_no_thread_token,
          "authority-namespace-context-restored", ERROR_ACCESS_DENIED);
      evidence.namespace_context_status = ERROR_SUCCESS;
    } catch (const Failure& failure) { evidence.namespace_context_status = failure.status; }
    if (restored == ERROR_SUCCESS && created != ERROR_SUCCESS && std::strcmp(stage, "lowbox.create-token") == 0) {
      // The failed production object has already been destroyed. This is an
      // equivalently derived token, observed only after the real failure; no
      // diagnostic result retries launch or changes that primary result.
      ObserveEquivalentToken(identity->token(), spec, &evidence.equivalent);
    }
    Check(created, stage);
    // A successfully created child remains suspended if observer teardown
    // failed. The normal cleanup path must stop it; never Resume under hooks.
    Check(restored, "restore-lowbox-iat-observer");
    Check(evidence.namespace_context_status, "authority-namespace-context-restored");
    const auto& creation = evidence.namespace_create;
    Require(creation.root_open_status == ERROR_SUCCESS && creation.privilege_status == ERROR_SUCCESS &&
        (creation.privilege_enabled & SE_PRIVILEGE_ENABLED) && creation.privilege_changes <= 1 &&
        creation.privilege_restore_status == ERROR_SUCCESS && creation.privilege_restored == creation.privilege_before &&
        creation.bind_status == ERROR_SUCCESS && creation.native_called == 1 && creation.native_status == 0 &&
        creation.context_restore_status == ERROR_SUCCESS && creation.pipe_create_called == 1 &&
        creation.pipe_create_status == 0 && creation.pipe_policy_status == ERROR_SUCCESS &&
        creation.pipe_policy.stage == storage::AuthorityNamespacePolicyStage::kComplete &&
        creation.pipe_policy.size_query_status == static_cast<LONG>(0xc0000023) &&
        creation.pipe_policy.allocation_bytes >= sizeof(SECURITY_DESCRIPTOR_RELATIVE) &&
        creation.pipe_policy.allocation_bytes <= 16384 && creation.pipe_policy.query_status >= 0 &&
        creation.pipe_policy.captured_bytes >= sizeof(SECURITY_DESCRIPTOR_RELATIVE),
        "authority-package-create-scoped-privilege-and-native-success", ERROR_INVALID_DATA);
    const auto& reseal = creation.reseal;
    Require(reseal.stage == storage::AuthorityNamespaceSealStage::kComplete && reseal.status == ERROR_SUCCESS &&
        reseal.privilege_status == ERROR_SUCCESS && reseal.bind_status == ERROR_SUCCESS &&
        (reseal.privilege_enabled & SE_PRIVILEGE_ENABLED) && reseal.privilege_changes <= 1 &&
        reseal.privilege_restored == reseal.privilege_before && reseal.privilege_restore_status == ERROR_SUCCESS &&
        reseal.context_restore_status == ERROR_SUCCESS && reseal.open_status == 0 && reseal.write_called == 1 &&
        reseal.write_status == 0 && reseal.close_called == 1 && reseal.close_status == ERROR_SUCCESS &&
        reseal.before_policy.stage == storage::AuthorityNamespacePolicyStage::kComplete &&
        reseal.after_policy.stage == storage::AuthorityNamespacePolicyStage::kComplete &&
        reseal.before_policy.requested_information == 0x1f && reseal.after_policy.requested_information == 0x1f &&
        creation.recheck.stage == storage::AuthorityNamespaceRecheckStage::kComplete,
        "authority-package-full-sacl-reseal-and-exact-readback", ERROR_INVALID_DATA);
    Require(WaitForSingleObject(child->process(), 0) == WAIT_TIMEOUT, "exact-child-suspended");
    BOOL contained = FALSE;
    Require(IsProcessInJob(child->process(), service_job.get(), &contained) && contained, "inherited-lowbox-job");
    HANDLE guest_token_raw = nullptr;
    Require(OpenProcessToken(child->process(), TOKEN_QUERY | TOKEN_DUPLICATE, &guest_token_raw),
        "session-objects-actual-suspended-lowbox-token");
    const Handle guest_token(guest_token_raw);
    VerifyNamespaceToken(guest_token.get(), identity->token(), spec);
    // Token-only negative: a distinct package derived from the same actual
    // private identity and capabilities. No wrapper, namespace preparation,
    // profile creation or alternate guest is executed for this control.
    container::Sid other_package;
    const HRESULT other_derived = DeriveAppContainerSidFromAppContainerName((package_name + L".Other").c_str(), &other_package.value);
    Require(SUCCEEDED(other_derived) && other_package.value && !EqualSid(other_package.value, package.value),
        "session-objects-alternate-package-sid", HRESULT_CODE(other_derived));
    auto other_capabilities = spec.capabilities;
    other_capabilities.AppContainerSid = other_package.value;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto native_lowbox = ntdll ? reinterpret_cast<CreateDiagnosticNativeLowbox>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateLowBoxToken"))) : nullptr;
    Handle other_token;
    const auto other_created = ObserveNativeLowbox(native_lowbox, identity->token(), other_capabilities, &other_token);
    Require(other_created.called && other_created.succeeded && other_created.returned_token && other_token,
        "session-objects-alternate-package-token-only", ERROR_ACCESS_DENIED);
    auto other_spec = spec;
    other_spec.capabilities = other_capabilities;
    VerifyNamespaceToken(other_token.get(), identity->token(), other_spec);
    DWORD isolation_status = ERROR_SUCCESS;
    const char* isolation_stage = "namespace-isolation";
    const auto observe_isolation = [&](auto probe) {
      try { probe(); }
      catch (const Failure& failure) {
        if (isolation_status == ERROR_SUCCESS) {
          isolation_status = failure.status;
          isolation_stage = failure.stage;
        }
      }
    };
    // Collect independent evidence while the real child remains suspended.
    // The first failure still prevents Resume, even if later probes succeed.
    observe_isolation([&] { VerifySessionObjectDelta(&session_objects, identity->sid(), 0x00020003, &evidence.session_objects); });
    observe_isolation([&] { VerifySessionObjectDelta(&app_container_objects, identity->sid(), 0x0000000f, &evidence.app_container_objects); });
    observe_isolation([&] { VerifySessionSubject(session_objects, guest_token.get(), config.session, &evidence.session_objects.subjects[2]); });
    observe_isolation([&] { VerifySessionSubject(app_container_objects, guest_token.get(), config.session, &evidence.app_container_objects.subjects[2]); });
    observe_isolation([&] { VerifySessionSubject(session_objects, other_token.get(), config.session, &evidence.session_objects.subjects[3]); });
    observe_isolation([&] { VerifySessionSubject(app_container_objects, other_token.get(), config.session, &evidence.app_container_objects.subjects[3]); });
    observe_isolation([&] {
      try { ObservePackageLeaf(package_object_path, *child, spec, guest_token.get(), other_token.get(), filtered.get(), &evidence.package_leaf); }
      catch (const Failure& failure) { evidence.package_leaf.status = failure.status; throw; }
    });
    observe_isolation([&] {
      ObserveProtectedPolicy(config.directory, spec, identity->token(), guest_token.get(), other_token.get(), &evidence.package_leaf);
    });
    observe_isolation([&] {
      try { ObservePackageReuse(identity->token(), spec, *child, package_object_path, &evidence.package_leaf); }
      catch (const Failure& failure) { evidence.package_leaf.status = failure.status; throw; }
    });
    collision_package_name = package_name + L".Collision";
    observe_isolation([&] {
      try {
        ObserveNamespaceCollision(*identity, filtered.get(), desktop_user, spec, collision_package_name,
            &collision_package, &evidence.namespace_collision);
      } catch (const Failure& failure) { evidence.namespace_collision.status = failure.status; throw; }
    });
    Check(isolation_status, isolation_stage);
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
  if (evidence.namespace_collision.profile_created && identity && evidence.cleanup == ERROR_SUCCESS) {
    if (ImpersonateLoggedOnUser(identity->token())) {
      const DWORD deleted = container::DeleteContainerProfile(collision_package_name);
      if (!RevertToSelf()) std::abort();
      record_cleanup(deleted, "delete-owned-collision-appcontainer");
    } else record_cleanup(GetLastError(), "impersonate-collision-profile-cleanup");
  }
  if (identity) {
    // A successful Create owns a loaded profile, and this is its sole explicit
    // unload call. Preserve the actual return separately from aggregate errors.
    evidence.profile_unload_attempted = 1;
    const DWORD unloaded = identity->UnloadProfile(evidence.cleanup);
    evidence.profile_unload_status = unloaded;
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
  evidence.profile_privilege_restore_status = cleanup.privilege_restore_error;
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
DWORD Controller(bool explicit_scm_control, bool explicit_lowbox_trace) {
  DWORD result = ERROR_SUCCESS;
  Service service;
  Handle process;
  Handle job;
  std::wstring directory, image;
  bool own_directory = false;
  bool worker_profile_release_verified = false;
  bool retained_windows_profile = false;
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
    if (explicit_lowbox_trace) command += L" --lowbox-iat-trace";
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
        std::memchr(evidence.equivalent.stage, 0, sizeof(evidence.equivalent.stage)) &&
        evidence.profile_identity_created <= 1 && evidence.profile_unload_attempted <= 1 &&
        evidence.profile_unload_attempted <= evidence.profile_identity_created,
        "validate-test-evidence", ERROR_INVALID_DATA);
    std::printf("Protected launch: private-token-preflight-image=%lu directory=%lu target-token=%lu caller-as-private=%lu caller-as-system=%lu\n",
        static_cast<unsigned long>(evidence.image_access), static_cast<unsigned long>(evidence.directory_access),
        static_cast<unsigned long>(evidence.target_token_access), static_cast<unsigned long>(evidence.private_caller_token_access),
        static_cast<unsigned long>(evidence.system_caller_token_access));
    std::printf("Protected launch: stage=%s win32=%lu cleanup_win32=%lu cleanup_stage=%s\n", evidence.stage,
        static_cast<unsigned long>(evidence.status), static_cast<unsigned long>(evidence.cleanup), evidence.cleanup_stage);
    std::printf("Protected launch: profile-identity-created=%lu own-unload-attempted=%lu own-unload-win32=%lu privilege-restore-win32=%lu\n",
        static_cast<unsigned long>(evidence.profile_identity_created), static_cast<unsigned long>(evidence.profile_unload_attempted),
        static_cast<unsigned long>(evidence.profile_unload_status), static_cast<unsigned long>(evidence.profile_privilege_restore_status));
    const auto& creation = evidence.namespace_create;
    std::printf("Protected package creation: root-open-win32=%lu privilege-enable-win32=%lu private-token-privilege-before=0x%08lx enabled=0x%08lx restored=0x%08lx changed-count=%lu privilege-restore-win32=%lu bind-win32=%lu native-called=%lu native-ntstatus=0x%08lx context-restore-win32=%lu\n",
        static_cast<unsigned long>(creation.root_open_status), static_cast<unsigned long>(creation.privilege_status),
        static_cast<unsigned long>(creation.privilege_before), static_cast<unsigned long>(creation.privilege_enabled),
        static_cast<unsigned long>(creation.privilege_restored), static_cast<unsigned long>(creation.privilege_changes),
        static_cast<unsigned long>(creation.privilege_restore_status), static_cast<unsigned long>(creation.bind_status),
        static_cast<unsigned long>(creation.native_called), static_cast<unsigned long>(creation.native_status),
        static_cast<unsigned long>(creation.context_restore_status));
    std::printf("Protected pipe creation: open-win32=%lu create-called=%lu create-ntstatus=0x%08lx policy-win32=%lu\n",
        static_cast<unsigned long>(creation.pipe_open_status), static_cast<unsigned long>(creation.pipe_create_called),
        static_cast<unsigned long>(creation.pipe_create_status), static_cast<unsigned long>(creation.pipe_policy_status));
    PrintPolicyReadback("package", creation.directory_policy);
    PrintPolicyReadback("pipe", creation.pipe_policy);
    PrintNamespaceSeal(creation.reseal);
    PrintNamespaceRecheck(creation.recheck);
    std::printf("Protected package creation: actual-process-privileges-unchanged=%lu actual-no-thread-token=%lu context-check-win32=%lu\n",
        static_cast<unsigned long>(evidence.namespace_process_privileges_unchanged),
        static_cast<unsigned long>(evidence.namespace_no_thread_token), static_cast<unsigned long>(evidence.namespace_context_status));
    worker_profile_release_verified = evidence.cleanup == ERROR_SUCCESS && evidence.profile_identity_created == 1 &&
        evidence.profile_unload_attempted == 1 && evidence.profile_unload_status == ERROR_SUCCESS &&
        evidence.profile_privilege_restore_status == ERROR_SUCCESS;
    PrintSessionObjects("BaseNamedObjects", 0x00020003, evidence.session_objects);
    PrintSessionObjects("AppContainerNamedObjects", 0x0000000f, evidence.app_container_objects);
    PrintPackageNamespace(evidence.package_leaf, evidence.namespace_collision);
    if (explicit_lowbox_trace) {
      std::puts("Protected lowbox IAT trace: trigger=explicit-opt-in scope=single-production-launch-call");
      lowbox_trace::PrintLowboxIatTrace(evidence.lowbox_trace);
    }
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
      // The independent SCM-only control reproduces final DeleteProfile
      // sharing/lock retention even without explicit profile loading. Classify
      // only that exact API result, after our own unload and exact lifetimes
      // are proved; an UnloadProfile or marker/provenance error stays fatal.
      retained_windows_profile = worker_profile_release_verified && worker_exit_verified &&
          cleanup_error == ERROR_SUCCESS && observed.attempts != 0 && profile == observed.api_status &&
          (profile == ERROR_SHARING_VIOLATION || profile == ERROR_LOCK_VIOLATION);
      if (!retained_windows_profile) record_cleanup(profile, "delete-owned-profile");
      std::printf("Protected launch: controller-profile-delete-attempts=%lu api=%lu hive=%lu\n",
          static_cast<unsigned long>(observed.attempts), static_cast<unsigned long>(observed.api_status),
          static_cast<unsigned long>(observed.hive_status));
      if (profile == ERROR_SUCCESS) {
        const auto remove_file = [&](const std::wstring& path, const char* operation) {
          if (!DeleteFileW(path.c_str())) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) record_cleanup(error, operation);
          }
        };
        remove_file(directory + L"\\private-data\\guest.bin", "delete-fixture-guest-file");
        if (!RemoveDirectoryW((directory + L"\\private-data").c_str())) {
          const DWORD error = GetLastError();
          if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) record_cleanup(error, "delete-fixture-private-data");
        }
        remove_file(directory + L"\\result.bin", "delete-fixture-evidence");
        remove_file(image, "delete-fixture-image");
        if (!RemoveDirectoryW(directory.c_str())) record_cleanup(GetLastError(), "delete-fixture-directory");
      } else if (retained_windows_profile) {
        // Keep the complete protected fixture directory, including profile.sid
        // and result.bin, as ownership/evidence. Do not claim profile deletion
        // or attempt RemoveDirectory on an intentionally retained marker.
        std::printf("Protected launch: retained-windows-profile api-win32=%lu attempts=%lu own-unload=success exact-worker-job-holder-stop=verified protected-artifacts=%ls\n",
            static_cast<unsigned long>(observed.api_status), static_cast<unsigned long>(observed.attempts), directory.c_str());
      }
    }
  }
  if (cleanup_error != ERROR_SUCCESS) std::fprintf(stderr, "Protected launch: controller-cleanup=%lu cleanup-stage=%s\n",
      static_cast<unsigned long>(cleanup_error), cleanup_stage);
  if (result == ERROR_SUCCESS) result = cleanup_error;
  if (result == ERROR_SUCCESS) {
    std::puts("PASS: actual private SCM identity, profile environment, lowbox child, desktop denial, exact owned-process teardown and matching profile unload");
    if (retained_windows_profile) std::puts("LIMITATION: Windows retained the owned service profile; final deletion failed with a recorded sharing/lock error and protected ownership artifacts remain.");
  }
  return result;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  DWORD control_exit = ERROR_INVALID_PARAMETER;
  if (profile_control::DispatchScmProfileControlService(argc, argv, &control_exit)) return static_cast<int>(control_exit);
  if (argc == 2 && std::wstring_view(argv[1]) == L"runtime-identity") return storage::RunRuntimeIdentityService();
  if (argc == 4 && std::wstring_view(argv[1]) == L"--prepare-desktop") return static_cast<int>(storage::PrepareRuntimeDesktop(argv[2], argv[3]));
  if (argc == 3 && std::wstring_view(argv[1]) == L"--guest") return static_cast<int>(Guest(argv[2]));
  if (argc >= 5 && argc <= 7 && std::wstring_view(argv[1]) == L"--test-service") {
    try {
      FixtureOptions options;
      Require(ParseFixtureOptions(argc, argv, 5, &options), "test-options-argument", ERROR_INVALID_PARAMETER);
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
      config.scm_profile_control = options.scm_profile_control;
      config.lowbox_iat_trace = options.lowbox_iat_trace;
      SERVICE_TABLE_ENTRYW entries[] = {{config.name.data(), ServiceMain}, {nullptr, nullptr}};
      return StartServiceCtrlDispatcherW(entries) ? 0 : static_cast<int>(GetLastError());
    } catch (...) { return 1; }
  }
  FixtureOptions options;
  if (ParseFixtureOptions(argc, argv, 1, &options)) return static_cast<int>(Controller(options.scm_profile_control, options.lowbox_iat_trace));
  return 2;
}
