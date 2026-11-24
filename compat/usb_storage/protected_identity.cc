// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "protected_identity.h"
#include "../framework/app_container.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <winternl.h>
#include <wtsapi32.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <new>
#include <string_view>
#include <type_traits>

namespace gesher::usb_storage {
namespace {
struct Close { void operator()(void* value) const { if (value) CloseHandle(value); } };
struct Local { void operator()(void* value) const { if (value) LocalFree(value); } };
struct ServiceClose { void operator()(SC_HANDLE value) const { if (value) CloseServiceHandle(value); } };
using Handle = std::unique_ptr<void, Close>;
using LocalMemory = std::unique_ptr<void, Local>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceClose>;

constexpr ACCESS_MASK kSessionObjectRead = READ_CONTROL | 0x0001 | 0x0002;
constexpr DWORD kNamespaceSecurityLimit = 65536;
constexpr DWORD kNamespaceAclLimit = 16384;
constexpr DWORD kNamespaceAceLimit = 128;

// Use the native Directory type, never a filesystem path or a DOS-device link.
// NtOpenDirectoryObject and NtQuery/SetSecurityObject have documented native
// ABIs. Resolve only exports of the already-loaded Windows modules.
struct SessionDirectory {
  using Open = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
  using QuerySecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG);
  using SetSecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
  using Error = ULONG(NTAPI*)(NTSTATUS);
  using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
  Open open = nullptr;
  QuerySecurity query = nullptr;
  SetSecurity set = nullptr;
  Error error = nullptr;
  Compare compare = nullptr;
  std::wstring path;
  UNICODE_STRING name{};
  OBJECT_ATTRIBUTES attributes{};

  DWORD Initialize(DWORD session) {
    const HMODULE native = GetModuleHandleW(L"ntdll.dll");
    const HMODULE base = GetModuleHandleW(L"kernelbase.dll");
    if (!native || !base) return ERROR_MOD_NOT_FOUND;
    open = reinterpret_cast<Open>(GetProcAddress(native, "NtOpenDirectoryObject"));
    query = reinterpret_cast<QuerySecurity>(GetProcAddress(native, "NtQuerySecurityObject"));
    set = reinterpret_cast<SetSecurity>(GetProcAddress(native, "NtSetSecurityObject"));
    error = reinterpret_cast<Error>(GetProcAddress(native, "RtlNtStatusToDosError"));
    compare = reinterpret_cast<Compare>(GetProcAddress(base, "CompareObjectHandles"));
    if (!open || !query || !set || !error || !compare) return ERROR_PROC_NOT_FOUND;
    path = L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects";
    if (!session || path.size() >= 128) return ERROR_INVALID_PARAMETER;
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(wchar_t));
    name.Buffer = path.data();
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &name;
    attributes.Attributes = 0x00001000;  // OBJ_DONT_REPARSE
    return ERROR_SUCCESS;
  }
  DWORD Status(NTSTATUS status) const noexcept {
    if (status >= 0) return ERROR_SUCCESS;
    const DWORD result = error(status);
    return result ? result : ERROR_GEN_FAILURE;
  }
  DWORD OpenHandle(ACCESS_MASK access, HANDLE* result) noexcept {
    *result = nullptr;
    const NTSTATUS status = open(result, access, &attributes);
    if (status < 0) {
      if (*result && *result != INVALID_HANDLE_VALUE) CloseHandle(*result);
      *result = nullptr;
      return Status(status);
    }
    if (!*result || *result == INVALID_HANDLE_VALUE) { *result = nullptr; return ERROR_INVALID_HANDLE; }
    return ERROR_SUCCESS;
  }
};

bool NamespaceSidWithin(PSID sid, const BYTE* begin, DWORD bytes) {
  const auto address = reinterpret_cast<ULONG_PTR>(sid);
  const auto first = reinterpret_cast<ULONG_PTR>(begin);
  if (!sid || address < first || address - first > bytes || bytes - (address - first) < 8) return false;
  const DWORD length = 8 + static_cast<DWORD>(*GetSidSubAuthorityCount(sid)) * static_cast<DWORD>(sizeof(DWORD));
  return length <= bytes - (address - first) && IsValidSid(sid) && GetLengthSid(sid) == length;
}
bool NamespaceAclWithin(PACL acl, const BYTE* begin, DWORD bytes) {
  if (!acl) return true;
  const auto address = reinterpret_cast<ULONG_PTR>(acl);
  const auto first = reinterpret_cast<ULONG_PTR>(begin);
  return address >= first && address - first <= bytes && bytes - (address - first) >= sizeof(ACL) &&
      acl->AclSize >= sizeof(ACL) && acl->AclSize <= kNamespaceAclLimit &&
      acl->AclSize <= bytes - (address - first) && acl->AceCount <= kNamespaceAceLimit && IsValidAcl(acl);
}
struct NamespaceSecurity {
  std::vector<DWORD> storage;
  PSID owner = nullptr, group = nullptr;
  PACL dacl = nullptr, label = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD Read(const SessionDirectory& api, HANDLE directory) {
    storage.resize(kNamespaceSecurityLimit / sizeof(DWORD));
    DWORD bytes = 0;
    const NTSTATUS status = api.query(directory, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, storage.data(), kNamespaceSecurityLimit, &bytes);
    if (status < 0) return api.Status(status);
    if (bytes < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || bytes > kNamespaceSecurityLimit) return ERROR_INVALID_SECURITY_DESCR;
    DWORD revision = 0;
    BOOL defaulted = FALSE, present = FALSE;
    if (!GetSecurityDescriptorControl(storage.data(), &control, &revision) || !(control & SE_SELF_RELATIVE) ||
        !GetSecurityDescriptorOwner(storage.data(), &owner, &defaulted) ||
        !GetSecurityDescriptorGroup(storage.data(), &group, &defaulted) ||
        !GetSecurityDescriptorDacl(storage.data(), &present, &dacl, &defaulted) || !present || !dacl) {
      return ERROR_INVALID_SECURITY_DESCR;
    }
    const auto* begin = reinterpret_cast<const BYTE*>(storage.data());
    if (!NamespaceSidWithin(owner, begin, bytes) || (group && !NamespaceSidWithin(group, begin, bytes)) ||
        !NamespaceAclWithin(dacl, begin, bytes)) return ERROR_INVALID_SECURITY_DESCR;
    if (!GetSecurityDescriptorSacl(storage.data(), &present, &label, &defaulted) ||
        !NamespaceAclWithin(label, begin, bytes)) return ERROR_INVALID_SECURITY_DESCR;
    return IsValidSecurityDescriptor(storage.data()) ? ERROR_SUCCESS : ERROR_INVALID_SECURITY_DESCR;
  }
};
bool SameNamespaceAcl(PACL a, PACL b) {
  if (!a || !b) return a == b;
  if (a->AclRevision != b->AclRevision || a->AceCount != b->AceCount) return false;
  for (DWORD i = 0; i < a->AceCount; ++i) {
    void* left = nullptr;
    void* right = nullptr;
    if (!GetAce(a, i, &left) || !GetAce(b, i, &right)) return false;
    const auto* x = static_cast<const ACE_HEADER*>(left);
    const auto* y = static_cast<const ACE_HEADER*>(right);
    if (x->AceSize != y->AceSize || std::memcmp(x, y, x->AceSize) != 0) return false;
  }
  return true;
}
bool SameNamespaceSecurity(const NamespaceSecurity& before, const NamespaceSecurity& after, PACL expected_dacl) {
  return EqualSid(before.owner, after.owner) &&
      ((!before.group || !after.group) ? before.group == after.group : EqualSid(before.group, after.group)) &&
      before.control == after.control && SameNamespaceAcl(before.label, after.label) &&
      SameNamespaceAcl(expected_dacl, after.dacl);
}
DWORD ValidateNamespacePolicy(const NamespaceSecurity& security, PSID private_user, bool* granted) {
  *granted = false;
  if (!IsWellKnownSid(security.owner, WinLocalSystemSid) &&
      !IsWellKnownSid(security.owner, WinBuiltinAdministratorsSid)) return ERROR_ACCESS_DENIED;
  for (DWORD i = 0; i < security.dacl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(security.dacl, i, &raw)) return GetLastError();
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    if ((ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceType != ACCESS_DENIED_ACE_TYPE) ||
        ace->Header.AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8) return ERROR_NOT_SUPPORTED;
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (!NamespaceSidWithin(sid, static_cast<const BYTE*>(raw), ace->Header.AceSize)) return ERROR_INVALID_ACL;
    if (EqualSid(sid, private_user)) {
      if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags ||
          (ace->Mask & ~kSessionObjectRead)) return ERROR_ACCESS_DENIED;
      if (ace->Mask == kSessionObjectRead) *granted = true;
    }
    // Selected-session desktop/logon principals may already have broader
    // Windows namespace rights. Preserve that ambient policy; only the exact
    // private SID's new grant is bounded here. The pinned root does not make
    // its pre-existing children trusted or authorize importing their objects.
  }
  return ERROR_SUCCESS;
}

SRWLOCK namespace_grant_lock = SRWLOCK_INIT;
struct NamespaceGrantLock {
  NamespaceGrantLock() { AcquireSRWLockExclusive(&namespace_grant_lock); }
  ~NamespaceGrantLock() { ReleaseSRWLockExclusive(&namespace_grant_lock); }
};
DWORD PrepareSessionObjects(HANDLE token, PSID private_user, DWORD session, HANDLE* result,
                            const char** startup_stage) {
  const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
  stage("identity.session-namespace-authority");
  HANDLE impersonation = nullptr;
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation)) {
    CloseHandle(impersonation);
    return ERROR_ACCESS_DENIED;
  }
  if (GetLastError() != ERROR_NO_TOKEN) return GetLastError();
  DWORD actual_session = 0, bytes = 0, contained = 0;
  TOKEN_TYPE type{};
  struct TokenUserBuffer { TOKEN_USER value; BYTE sid[SECURITY_MAX_SID_SIZE]; } actual{};
  stage("identity.session-namespace-token");
  if (!GetTokenInformation(token, TokenUser, &actual, sizeof(actual), &bytes) ||
      !GetTokenInformation(token, TokenSessionId, &actual_session, sizeof(actual_session), &bytes) ||
      !GetTokenInformation(token, TokenType, &type, sizeof(type), &bytes) ||
      !GetTokenInformation(token, TokenIsAppContainer, &contained, sizeof(contained), &bytes)) return GetLastError();
  if (!NamespaceSidWithin(actual.value.User.Sid, reinterpret_cast<const BYTE*>(&actual), sizeof(actual)) ||
      !EqualSid(actual.value.User.Sid, private_user) || !session || actual_session != session ||
      type != TokenPrimary || contained) return ERROR_ACCESS_DENIED;
  SessionDirectory api;
  stage("identity.session-namespace-api");
  DWORD status = api.Initialize(session);
  if (status != ERROR_SUCCESS) return status;
  NamespaceGrantLock lock;
  HANDLE opened = nullptr;
  stage("identity.session-namespace-open");
  status = api.OpenHandle(kSessionObjectRead | WRITE_DAC, &opened);
  if (status != ERROR_SUCCESS) return status;
  const Handle directory(opened);
  NamespaceSecurity before;
  stage("identity.session-namespace-security");
  status = before.Read(api, opened);
  if (status != ERROR_SUCCESS) return status;
  bool granted = false;
  stage("identity.session-namespace-policy");
  status = ValidateNamespacePolicy(before, private_user, &granted);
  if (status != ERROR_SUCCESS) return status;
  if (!granted) {
    ACL_SIZE_INFORMATION information{};
    if (!GetAclInformation(before.dacl, &information, sizeof(information), AclSizeInformation)) return GetLastError();
    const DWORD added = static_cast<DWORD>(offsetof(ACCESS_ALLOWED_ACE, SidStart)) + GetLengthSid(private_user);
    if (information.AceCount >= kNamespaceAceLimit || information.AclBytesInUse > kNamespaceAclLimit - added) return ERROR_MORE_DATA;
    const DWORD capacity = information.AclBytesInUse + added;
    std::vector<DWORD> acl_storage((capacity + sizeof(DWORD) - 1) / sizeof(DWORD));
    auto* updated = reinterpret_cast<PACL>(acl_storage.data());
    if (!InitializeAcl(updated, capacity, before.dacl->AclRevision)) return GetLastError();
    for (DWORD i = 0; i < before.dacl->AceCount; ++i) {
      void* ace = nullptr;
      if (!GetAce(before.dacl, i, &ace) ||
          !AddAce(updated, before.dacl->AclRevision, MAXDWORD, ace, static_cast<ACE_HEADER*>(ace)->AceSize)) return GetLastError();
    }
    if (!AddAccessAllowedAceEx(updated, before.dacl->AclRevision, 0, kSessionObjectRead, private_user)) return GetLastError();
    NamespaceSecurity current;
    stage("identity.session-namespace-reread");
    status = current.Read(api, opened);
    if (status != ERROR_SUCCESS) return status;
    if (!SameNamespaceSecurity(before, current, before.dacl)) return ERROR_RETRY;
    SECURITY_DESCRIPTOR descriptor{};
    constexpr SECURITY_DESCRIPTOR_CONTROL copied = SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED | SE_DACL_AUTO_INHERIT_REQ;
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, updated, (before.control & SE_DACL_DEFAULTED) != 0) ||
        !SetSecurityDescriptorControl(&descriptor, copied,
            static_cast<SECURITY_DESCRIPTOR_CONTROL>(before.control & copied))) return GetLastError();
    stage("identity.session-namespace-grant");
    status = api.Status(api.set(opened, DACL_SECURITY_INFORMATION, &descriptor));
    if (status != ERROR_SUCCESS) return status;
    NamespaceSecurity after;
    stage("identity.session-namespace-readback");
    status = after.Read(api, opened);
    if (status != ERROR_SUCCESS) return status;
    if (!SameNamespaceSecurity(before, after, updated)) return ERROR_ACCESS_DENIED;
  }
  // This one non-inheriting read grant lasts for the selected Windows session
  // object. We never restore a stale whole DACL on teardown. Our lock serializes
  // our grants; the immediate rereads do not claim an OS-wide atomic ACL merge.
  stage("identity.session-namespace-private-open");
  if (!ImpersonateLoggedOnUser(token)) return GetLastError();
  HANDLE private_open = nullptr;
  status = api.OpenHandle(kSessionObjectRead, &private_open);
  const BOOL same = status == ERROR_SUCCESS && api.compare(opened, private_open);
  if (!RevertToSelf()) std::abort();
  const Handle private_directory(private_open);
  if (status != ERROR_SUCCESS) return status;
  if (!same) return ERROR_NOT_SAME_OBJECT;
  stage("identity.session-namespace-retain");
  HANDLE retained = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), opened, GetCurrentProcess(), &retained,
      kSessionObjectRead, FALSE, 0)) return GetLastError();
  Handle retained_owner(retained);
  DWORD flags = 0;
  if (!GetHandleInformation(retained, &flags)) return GetLastError();
  if ((flags & HANDLE_FLAG_INHERIT) || !api.compare(opened, retained)) return ERROR_ACCESS_DENIED;
  *result = retained_owner.release();
  return ERROR_SUCCESS;
}

class ProfilePrivileges {
 public:
  explicit ProfilePrivileges(ProtectedIdentityCleanup* cleanup) : cleanup_(cleanup) {}
  ~ProfilePrivileges() { Restore(); }
  DWORD Enable() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &raw)) return GetLastError();
    token_.reset(raw);
    alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD bytes = 0;
    if (!GetTokenInformation(raw, TokenUser, user, sizeof(user), &bytes)) return GetLastError();
    if (!IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid)) return ERROR_ACCESS_DENIED;
    struct TwoPrivileges { DWORD count; LUID_AND_ATTRIBUTES entries[2]; } requested{2, {}};
    if (!LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &requested.entries[0].Luid) ||
        !LookupPrivilegeValueW(nullptr, L"SeRestorePrivilege", &requested.entries[1].Luid)) return GetLastError();
    for (auto& privilege : requested.entries) privilege.Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted = AdjustTokenPrivileges(raw, FALSE, reinterpret_cast<TOKEN_PRIVILEGES*>(&requested),
        sizeof(previous_), reinterpret_cast<TOKEN_PRIVILEGES*>(previous_), &bytes);
    const DWORD error = GetLastError();
    if (!adjusted) return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
    changed_ = true;
    // TRUE alone does not mean both requested privileges were assigned.
    return error;
  }
  DWORD Restore() {
    if (!changed_) return ERROR_SUCCESS;
    changed_ = false;
    SetLastError(ERROR_SUCCESS);
    const BOOL restored = AdjustTokenPrivileges(token_.get(), FALSE,
        reinterpret_cast<TOKEN_PRIVILEGES*>(previous_), 0, nullptr, nullptr);
    DWORD error = GetLastError();
    if (!restored && error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
    if (error != ERROR_SUCCESS && cleanup_->privilege_restore_error == ERROR_SUCCESS) {
      cleanup_->privilege_restore_error = error;
    }
    return error;
  }
 private:
  Handle token_;
  ProtectedIdentityCleanup* cleanup_;
  alignas(TOKEN_PRIVILEGES) BYTE previous_[offsetof(TOKEN_PRIVILEGES, Privileges) + 2 * sizeof(LUID_AND_ATTRIBUTES)]{};
  bool changed_ = false;
};

bool Same(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
      b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
DWORD Sid(std::vector<BYTE>* sid) {
  DWORD bytes = 0, domain_bytes = 0;
  SID_NAME_USE use{};
  LookupAccountNameW(nullptr, kRuntimeIdentityAccount, nullptr, &bytes, nullptr, &domain_bytes, &use);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > SECURITY_MAX_SID_SIZE || !bytes ||
      domain_bytes > 256) return ERROR_INVALID_SID;
  sid->resize(bytes);
  std::vector<wchar_t> domain(domain_bytes);
  if (!LookupAccountNameW(nullptr, kRuntimeIdentityAccount, sid->data(), &bytes,
      domain.data(), &domain_bytes, &use)) return GetLastError();
  return IsValidSid(sid->data()) ? ERROR_SUCCESS : ERROR_INVALID_SID;
}
DWORD Validate(SC_HANDLE service, const std::wstring& executable) {
  DWORD bytes = 0;
  QueryServiceConfigW(service, nullptr, 0, &bytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 16384 || !bytes) return ERROR_INVALID_DATA;
  std::vector<BYTE> buffer(bytes);
  auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
  if (!QueryServiceConfigW(service, config, bytes, &bytes)) return GetLastError();
  const std::wstring expected = L"\"" + executable + L"\" runtime-identity";
  if (!config->lpBinaryPathName || !Same(config->lpBinaryPathName, expected) ||
      !config->lpServiceStartName || !Same(config->lpServiceStartName, kRuntimeIdentityAccount) ||
      config->dwServiceType != SERVICE_WIN32_OWN_PROCESS || config->dwStartType != SERVICE_DEMAND_START) {
    return ERROR_ACCESS_DENIED;
  }
  QueryServiceObjectSecurity(service, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            nullptr, 0, &bytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 16384 || !bytes) return ERROR_INVALID_ACL;
  buffer.resize(bytes);
  if (!QueryServiceObjectSecurity(service, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      buffer.data(), bytes, &bytes)) return GetLastError();
  PSID owner = nullptr;
  PACL acl = nullptr;
  BOOL defaulted = FALSE, present = FALSE;
  if (!GetSecurityDescriptorOwner(buffer.data(), &owner, &defaulted) ||
      !GetSecurityDescriptorDacl(buffer.data(), &present, &acl, &defaulted) || !present || !acl ||
      (!IsWellKnownSid(owner, WinLocalSystemSid) && !IsWellKnownSid(owner, WinBuiltinAdministratorsSid))) {
    return ERROR_ACCESS_DENIED;
  }
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(acl, i, &raw)) return GetLastError();
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0) {
      return ERROR_ACCESS_DENIED;
    }
    PSID principal = const_cast<DWORD*>(&ace->SidStart);
    if (!IsWellKnownSid(principal, WinLocalSystemSid) &&
        !IsWellKnownSid(principal, WinBuiltinAdministratorsSid)) return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
}

DWORD QueryService(SC_HANDLE service, SERVICE_STATUS_PROCESS* status) {
  DWORD size = 0;
  return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(status),
      sizeof(*status), &size) ? ERROR_SUCCESS : GetLastError();
}

DWORD GrantUserObject(HANDLE object, PSID sid, ACCESS_MASK access) {
  SECURITY_INFORMATION information = DACL_SECURITY_INFORMATION;
  DWORD size = 0;
  GetUserObjectSecurity(object, &information, nullptr, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || size > 65536) return ERROR_INVALID_ACL;
  std::vector<BYTE> bytes(size);
  if (!GetUserObjectSecurity(object, &information, bytes.data(), size, &size)) return GetLastError();
  PACL acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(bytes.data(), &present, &acl, &defaulted) || !present || !acl) {
    return ERROR_INVALID_ACL;
  }
  EXPLICIT_ACCESSW entry{};
  entry.grfAccessMode = GRANT_ACCESS;
  entry.grfAccessPermissions = access;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.ptstrName = static_cast<wchar_t*>(sid);
  PACL updated = nullptr;
  const DWORD status = SetEntriesInAclW(1, &entry, acl, &updated);
  if (status != ERROR_SUCCESS) return status;
  const LocalMemory release(updated);
  SECURITY_DESCRIPTOR descriptor{};
  if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&descriptor, TRUE, updated, FALSE) ||
      !SetUserObjectSecurity(object, &information, &descriptor)) return GetLastError();
  return ERROR_SUCCESS;
}

struct IdentityServiceState { SERVICE_STATUS_HANDLE handle = nullptr; HANDLE stop = nullptr; };
void Report(IdentityServiceState& context, DWORD state, DWORD error = ERROR_SUCCESS) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
  status.dwWin32ExitCode = error;
  SetServiceStatus(context.handle, &status);
}
DWORD WINAPI Control(DWORD operation, DWORD, void*, void* value) {
  auto& context = *static_cast<IdentityServiceState*>(value);
  if (operation == SERVICE_CONTROL_STOP) { SetEvent(context.stop); return ERROR_SUCCESS; }
  return operation == SERVICE_CONTROL_INTERROGATE ? ERROR_SUCCESS : ERROR_CALL_NOT_IMPLEMENTED;
}
void WINAPI Main(DWORD, wchar_t**) {
  const Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  IdentityServiceState context{nullptr, stop.get()};
  context.handle = RegisterServiceCtrlHandlerExW(kRuntimeIdentityService, Control, &context);
  if (!context.handle) return;
  HANDLE raw = nullptr;
  DWORD result = ERROR_ACCESS_DENIED;
  if (stop && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
    const Handle token(raw);
    std::vector<BYTE> expected;
    alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD count = 0;
    result = Sid(&expected);
    if (result == ERROR_SUCCESS && (!GetTokenInformation(raw, TokenUser, user, sizeof(user), &count) ||
        !EqualSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, expected.data()))) result = ERROR_ACCESS_DENIED;
  }
  if (result == ERROR_SUCCESS) {
    Report(context, SERVICE_RUNNING);
    if (WaitForSingleObject(stop.get(), INFINITE) != WAIT_OBJECT_0) result = GetLastError();
  }
  Report(context, SERVICE_STOPPED, result);
}
}  // namespace

DWORD InstallRuntimeIdentityService(const std::wstring& executable) {
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (!manager) return GetLastError();
  const std::wstring command = L"\"" + executable + L"\" runtime-identity";
  Service service(CreateServiceW(manager.get(), kRuntimeIdentityService, L"Gesher protected runtime identity",
      SERVICE_CHANGE_CONFIG | READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(),
      nullptr, nullptr, nullptr, kRuntimeIdentityAccount, nullptr));
  if (!service) {
    if (GetLastError() != ERROR_SERVICE_EXISTS) return GetLastError();
    service.reset(OpenServiceW(manager.get(), kRuntimeIdentityService, SERVICE_QUERY_CONFIG | READ_CONTROL));
    return service ? Validate(service.get(), executable) : GetLastError();
  }
  PSECURITY_DESCRIPTOR raw = nullptr;
  DWORD status = ERROR_SUCCESS;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &raw, nullptr)) status = GetLastError();
  const LocalMemory security(raw);
  if (status == ERROR_SUCCESS && !SetServiceObjectSecurity(service.get(),
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, raw)) status = GetLastError();
  wchar_t privileges[] = L"SeChangeNotifyPrivilege\0";
  SERVICE_REQUIRED_PRIVILEGES_INFOW restricted{privileges};
  if (status == ERROR_SUCCESS && !ChangeServiceConfig2W(service.get(),
      SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &restricted)) status = GetLastError();
  if (status != ERROR_SUCCESS) DeleteService(service.get());
  return status;
}

DWORD UninstallRuntimeIdentityService(const std::wstring& executable) {
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) return GetLastError();
  const Service service(OpenServiceW(manager.get(), kRuntimeIdentityService,
      SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | READ_CONTROL | DELETE));
  if (!service) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : GetLastError();
  DWORD status = Validate(service.get(), executable);
  if (status != ERROR_SUCCESS) return status;
  SERVICE_STATUS_PROCESS state{};
  status = QueryService(service.get(), &state);
  if (status != ERROR_SUCCESS) return status;
  const ULONGLONG deadline = GetTickCount64() + 15000;
  Handle holder;
  DWORD holder_pid = 0;
  const auto valid_pid_state = [](DWORD value) {
    return value == SERVICE_RUNNING || value == SERVICE_PAUSE_PENDING ||
        value == SERVICE_PAUSED || value == SERVICE_CONTINUE_PENDING;
  };
  if (state.dwCurrentState != SERVICE_STOPPED) {
    // SCM does not promise a valid PID in START/STOP_PENDING. Do not open a
    // stale PID or claim that an unverified process has finished.
    if (!valid_pid_state(state.dwCurrentState)) return ERROR_BUSY;
    holder_pid = state.dwProcessId;
    if (!holder_pid) return ERROR_INVALID_DATA;
    holder.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, holder_pid));
    if (!holder) return GetLastError();
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(holder.get(), 0, image.data(), &length)) return GetLastError();
    image.resize(length);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(holder.get(), &created, &exited, &kernel, &user)) return GetLastError();
    if (!Same(image, executable) || (!created.dwLowDateTime && !created.dwHighDateTime)) return ERROR_ACCESS_DENIED;
    // The handle pins this creation, independently of any later PID reuse.
    status = QueryService(service.get(), &state);
    if (status != ERROR_SUCCESS) return status;
    if (state.dwCurrentState == SERVICE_START_PENDING ||
        (valid_pid_state(state.dwCurrentState) && state.dwProcessId != holder_pid)) return ERROR_PROCESS_ABORTED;
  }
  if (state.dwCurrentState != SERVICE_STOPPED) {
    SERVICE_STATUS stopping{};
    if (state.dwCurrentState != SERVICE_STOP_PENDING &&
        !ControlService(service.get(), SERVICE_CONTROL_STOP, &stopping) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return GetLastError();
    do {
      if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
      Sleep(25);
      status = QueryService(service.get(), &state);
      if (status != ERROR_SUCCESS) return status;
      if (state.dwCurrentState == SERVICE_START_PENDING ||
          (valid_pid_state(state.dwCurrentState) && state.dwProcessId != holder_pid)) return ERROR_PROCESS_ABORTED;
    } while (state.dwCurrentState != SERVICE_STOPPED);
  }
  if (holder) {
    // SERVICE_STOPPED can precede Main's return and release of its last token
    // references. Wait on the retained object before allowing profile cleanup.
    const ULONGLONG now = GetTickCount64();
    const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    const DWORD waited = WaitForSingleObject(holder.get(), remaining);
    if (waited != WAIT_OBJECT_0) return waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
  }
  return DeleteService(service.get()) ? ERROR_SUCCESS : GetLastError();
}

int RunRuntimeIdentityService() {
  wchar_t name[] = L"GesherProtectedRuntime";
  SERVICE_TABLE_ENTRYW dispatch[] = {{name, Main}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(dispatch) ? 0 : 1;
}

DWORD PrepareRuntimeDesktop(const wchar_t* runtime_sid, const wchar_t* capability_sid) {
  PSID user = nullptr, capability = nullptr;
  if (!runtime_sid || !capability_sid || !ConvertStringSidToSidW(runtime_sid, &user)) return ERROR_INVALID_SID;
  const LocalMemory own_user(user);
  if (!ConvertStringSidToSidW(capability_sid, &capability)) return ERROR_INVALID_SID;
  const LocalMemory own_capability(capability);
  constexpr ACCESS_MASK station = WINSTA_ACCESSCLIPBOARD | WINSTA_ACCESSGLOBALATOMS |
      WINSTA_ENUMDESKTOPS | WINSTA_ENUMERATE | WINSTA_READATTRIBUTES | WINSTA_READSCREEN;
  constexpr ACCESS_MASK desktop = DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW |
      DESKTOP_ENUMERATE | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS;
  for (PSID sid : {user, capability}) {
    DWORD result = GrantUserObject(GetProcessWindowStation(), sid, station);
    if (result == ERROR_SUCCESS) result = GrantUserObject(GetThreadDesktop(GetCurrentThreadId()), sid, desktop);
    if (result != ERROR_SUCCESS) return result;
  }
  return ERROR_SUCCESS;
}

ProtectedIdentity::~ProtectedIdentity() {
  UnloadProfile();
  if (session_objects_) CloseHandle(session_objects_);
  if (token_) CloseHandle(token_);
}

DWORD ProtectedIdentity::AppendProfileEnvironment(std::vector<std::wstring>* environment) const try {
  if (!environment || !profile_ || unload_attempted_) return ERROR_INVALID_PARAMETER;
  struct Folder { const wchar_t* name; const KNOWNFOLDERID* id; };
  const Folder folders[] = {{L"USERPROFILE", &FOLDERID_Profile},
      {L"LOCALAPPDATA", &FOLDERID_LocalAppData}, {L"APPDATA", &FOLDERID_RoamingAppData}};
  std::vector<std::wstring> additions;
  for (const auto& folder : folders) {
    for (const auto& existing : *environment) {
      const size_t equal = existing.find(L'=');
      if (equal != std::wstring::npos && Same(std::wstring_view(existing).substr(0, equal), folder.name)) {
        return ERROR_INVALID_PARAMETER;
      }
    }
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(*folder.id, 0, token_, &raw);
    const auto release = [](wchar_t* value) { CoTaskMemFree(value); };
    const std::unique_ptr<wchar_t, decltype(release)> path_owner(raw, release);
    if (result != S_OK) return HRESULT_CODE(result) ? HRESULT_CODE(result) : ERROR_GEN_FAILURE;
    if (!raw) return ERROR_INVALID_DATA;
    const size_t size = std::wcslen(raw);
    if (size < 4 || size > 8192 || raw[1] != L':' || raw[2] != L'\\' ||
        (raw[0] | 0x20) < L'a' || (raw[0] | 0x20) > L'z') return ERROR_INVALID_DATA;
    const std::wstring path(raw, size);
    if (path.find_first_of(L"\":/", 2) != std::wstring::npos) return ERROR_INVALID_DATA;
    additions.push_back(std::wstring(folder.name) + L"=" + path);
  }
  environment->insert(environment->end(), additions.begin(), additions.end());
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }

DWORD ProtectedIdentity::UnloadProfile(DWORD users_stopped) {
  if (!profile_ || unload_attempted_) return cleanup_->profile_error.load();
  unload_attempted_ = true;
  DWORD status = users_stopped != ERROR_SUCCESS ? users_stopped : cleanup_->profile_error.load();
  ProfilePrivileges privileges(cleanup_);
  if (status == ERROR_SUCCESS) status = privileges.Enable();
  if (status == ERROR_SUCCESS) {
    if (!UnloadUserProfile(token_, profile_)) status = GetLastError();
    else profile_ = nullptr;
  }
  const DWORD restore = privileges.Restore();
  if (status != ERROR_SUCCESS) {
    DWORD expected = ERROR_SUCCESS;
    cleanup_->profile_error.compare_exchange_strong(expected, status);
  }
  return status != ERROR_SUCCESS ? status : restore;
}

DWORD ProtectedIdentity::Create(const std::wstring& executable, DWORD desktop_session,
    std::unique_ptr<ProtectedIdentity>* result, const char** startup_stage,
    ProtectedIdentityCleanup* cleanup) try {
  const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
  stage("identity.arguments");
  if (!desktop_session || !result || *result || !cleanup) return ERROR_INVALID_PARAMETER;
  stage("identity.authority-token");
  HANDLE raw_self = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_self)) return GetLastError();
  const Handle self(raw_self);
  alignas(TOKEN_USER) BYTE system[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  if (!GetTokenInformation(raw_self, TokenUser, system, sizeof(system), &bytes) ||
      !IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(system)->User.Sid, WinLocalSystemSid)) return ERROR_ACCESS_DENIED;
  stage("identity.open-scm");
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) return GetLastError();
  stage("identity.open-service");
  const Service service(OpenServiceW(manager.get(), kRuntimeIdentityService,
      SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | READ_CONTROL));
  if (!service) return GetLastError();
  stage("identity.validate-service");
  DWORD status = Validate(service.get(), executable);
  if (status != ERROR_SUCCESS) return status;
  stage("identity.start-service");
  if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) return GetLastError();
  SERVICE_STATUS_PROCESS state{};
  const ULONGLONG deadline = GetTickCount64() + 15000;
  stage("identity.wait-service");
  do {
    status = QueryService(service.get(), &state);
    if (status != ERROR_SUCCESS) return status;
    if (state.dwCurrentState == SERVICE_RUNNING) break;
    if (state.dwCurrentState == SERVICE_STOPPED) return state.dwWin32ExitCode ? state.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
    if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
    Sleep(25);
  } while (true);
  if (!state.dwProcessId || state.dwServiceType != SERVICE_WIN32_OWN_PROCESS) return ERROR_ACCESS_DENIED;
  stage("identity.pin-process");
  const Handle holder(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, state.dwProcessId));
  if (!holder) return GetLastError();
  std::wstring image(32768, L'\0');
  DWORD length = static_cast<DWORD>(image.size());
  stage("identity.process-image");
  if (!QueryFullProcessImageNameW(holder.get(), 0, image.data(), &length)) return GetLastError();
  image.resize(length);
  if (!Same(image, executable) || WaitForSingleObject(holder.get(), 0) != WAIT_TIMEOUT) return ERROR_ACCESS_DENIED;
  HANDLE raw_token = nullptr;
  stage("identity.open-primary-token");
  if (!OpenProcessToken(holder.get(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw_token)) return GetLastError();
  const Handle token(raw_token);
  auto identity = std::unique_ptr<ProtectedIdentity>(new ProtectedIdentity(cleanup));
  stage("identity.resolve-account-sid");
  status = Sid(&identity->sid_);
  if (status != ERROR_SUCCESS) return status;
  alignas(TOKEN_USER) BYTE actual[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  stage("identity.verify-token-user");
  if (!GetTokenInformation(raw_token, TokenUser, actual, sizeof(actual), &bytes) ||
      !EqualSid(reinterpret_cast<TOKEN_USER*>(actual)->User.Sid, identity->sid())) return ERROR_ACCESS_DENIED;
  SERVICE_STATUS_PROCESS second{};
  stage("identity.recheck-scm-process");
  status = QueryService(service.get(), &second);
  if (status != ERROR_SUCCESS) return status;
  if (second.dwCurrentState != SERVICE_RUNNING || second.dwProcessId != state.dwProcessId) return ERROR_PROCESS_ABORTED;
  stage("identity.duplicate-primary-token");
  if (!DuplicateTokenEx(raw_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary,
                         &identity->token_)) return GetLastError();
  identity->session_ = desktop_session;
  stage("identity.set-desktop-session");
  if (!SetTokenInformation(identity->token_, TokenSessionId, &desktop_session, sizeof(desktop_session))) return GetLastError();
  PSID medium = nullptr;
  stage("identity.set-integrity");
  if (!ConvertStringSidToSidW(L"S-1-16-8192", &medium)) return GetLastError();
  const LocalMemory medium_owner(medium);
  TOKEN_MANDATORY_LABEL integrity{{medium, SE_GROUP_INTEGRITY}};
  if (!SetTokenInformation(identity->token_, TokenIntegrityLevel, &integrity,
      static_cast<DWORD>(sizeof(integrity)) + GetLengthSid(medium))) return GetLastError();
  // SCM may hold its own service-account profile load. Provision the local
  // profile for the exact verified TokenUser SID and retain our independent
  // LoadUserProfile/UnloadUserProfile pair for the AppContainer API lifetime.
  // Releasing our pair does not prove SCM's or another component's references
  // are gone. The bare name is only the profile directory's base name;
  // LoadUserProfile still receives the pinned token.
  stage("identity.profile-sid");
  wchar_t* sid_text = nullptr;
  if (!ConvertSidToStringSidW(identity->sid(), &sid_text)) return GetLastError();
  const LocalMemory own_sid_text(sid_text);
  wchar_t account[] = L"GesherProtectedRuntime";
  wchar_t profile_path[MAX_PATH]{};
  stage("identity.enable-profile-privileges");
  ProfilePrivileges privileges(cleanup);
  status = privileges.Enable();
  if (status != ERROR_SUCCESS) return status;
  stage("identity.create-profile");
  const HRESULT created = CreateProfile(sid_text, account, profile_path,
      static_cast<DWORD>(std::size(profile_path)));
  if (created != S_OK && created != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
    return HRESULT_CODE(created) ? HRESULT_CODE(created) : ERROR_GEN_FAILURE;
  }
  PROFILEINFOW profile{};
  profile.dwSize = sizeof(profile);
  profile.dwFlags = PI_NOUI;
  profile.lpUserName = account;
  stage("identity.load-profile");
  if (!LoadUserProfileW(identity->token_, &profile)) return GetLastError();
  identity->profile_ = profile.hProfile;
  stage("identity.restore-profile-privileges");
  status = privileges.Restore();
  if (status != ERROR_SUCCESS) return status;
  status = PrepareSessionObjects(identity->token_, identity->sid(), desktop_session,
      &identity->session_objects_, startup_stage);
  if (status != ERROR_SUCCESS) return status;
  *result = std::move(identity);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
}  // namespace gesher::usb_storage
