// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "authority_launch.h"

#include <aclapi.h>
#include <sddl.h>
#include <winternl.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace gesher::usb_storage {
namespace {

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
struct LocalFreeOwner {
  void operator()(void* pointer) const { LocalFree(pointer); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;
using LocalMemory = std::unique_ptr<void, LocalFreeOwner>;

constexpr wchar_t kObjectPolicy[] = L"O:SYG:SYD:P(A;;0x001fffff;;;SY)";
static_assert(PROCESS_ALL_ACCESS == 0x001fffff && THREAD_ALL_ACCESS == 0x001fffff);
// The restricted authority working token isn't an application. Its default
// must not give the private user implicit owner rights on working objects.
constexpr wchar_t kDefaultPolicy[] = L"D:P(D;;WDWO;;;OW)(A;;GA;;;SY)";
constexpr DWORD kMaximumCapabilities = 64;

template <class T>
bool Query(HANDLE token, TOKEN_INFORMATION_CLASS kind, T* result, DWORD size = sizeof(T)) {
  DWORD received = 0;
  return GetTokenInformation(token, kind, result, size, &received) != FALSE;
}

struct Identity {
  // The typed headers provide the Win32 alignment naturally. Their trailing
  // SID storage also permits ordinary structure padding without an explicitly
  // aligned byte-array member (which MSVC diagnoses as C4324).
  struct UserBuffer { TOKEN_USER value; BYTE sid[SECURITY_MAX_SID_SIZE]; };
  struct IntegrityBuffer { TOKEN_MANDATORY_LABEL value; BYTE sid[SECURITY_MAX_SID_SIZE]; };
  TOKEN_STATISTICS stats{};
  DWORD session = 0;
  DWORD contained = 0;
  DWORD elevated = 0;
  DWORD ui_access = 0;
  UserBuffer user{};
  IntegrityBuffer integrity{};

  bool Read(HANDLE token) {
    return Query(token, TokenStatistics, &stats) && Query(token, TokenSessionId, &session) &&
        Query(token, TokenIsAppContainer, &contained) && Query(token, TokenElevation, &elevated) &&
        Query(token, TokenUIAccess, &ui_access) && Query(token, TokenUser, &user) &&
        Query(token, TokenIntegrityLevel, &integrity) &&
        IsValidSid(User()) && IsValidSid(Integrity());
  }
  PSID User() const { return user.value.User.Sid; }
  PSID Integrity() const {
    return integrity.value.Label.Sid;
  }
  bool MatchesDesktop(const AuthorityLaunchSpec& spec, TOKEN_TYPE type = TokenPrimary) const {
    return stats.TokenType == type && session == spec.desktop_session &&
        !elevated && !ui_access && EqualSid(User(), spec.desktop_user) &&
        !IsWellKnownSid(User(), WinLocalSystemSid) && !IsWellKnownSid(User(), WinLocalServiceSid) &&
        !IsWellKnownSid(User(), WinNetworkServiceSid) && !IsWellKnownSid(User(), WinAnonymousSid);
  }
};

bool LocalSystemWorker() {
  HANDLE token = nullptr;
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
    CloseHandle(token);
    return false;
  }
  if (GetLastError() != ERROR_NO_TOKEN ||
      !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  const Handle owner(token);
  alignas(TOKEN_USER) BYTE info[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  return Query(token, TokenUser, info, sizeof(info)) &&
      IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(info)->User.Sid, WinLocalSystemSid);
}

enum class SessionObjectRoot { kBase, kAppContainer };
DWORD VerifySessionObjects(const AuthorityLaunchSpec& spec, SessionObjectRoot root, Handle* retained,
                           ACCESS_MASK access = READ_CONTROL | 0x0001 | 0x0002) {
  HANDLE expected = nullptr;
  const wchar_t* leaf = nullptr;
  switch (root) {
    case SessionObjectRoot::kBase:
      expected = spec.session_objects;
      leaf = L"BaseNamedObjects";
      break;
    case SessionObjectRoot::kAppContainer:
      expected = spec.app_container_objects;
      leaf = L"AppContainerNamedObjects";
      break;
    default:
      return ERROR_INVALID_PARAMETER;
  }
  DWORD flags = 0;
  if (!GetHandleInformation(expected, &flags)) return GetLastError();
  if (flags & HANDLE_FLAG_INHERIT) return ERROR_ACCESS_DENIED;
  using OpenDirectory = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
  using NativeError = ULONG(NTAPI*)(NTSTATUS);
  using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
  const HMODULE native = GetModuleHandleW(L"ntdll.dll");
  const HMODULE base = GetModuleHandleW(L"kernelbase.dll");
  if (!native || !base) return ERROR_MOD_NOT_FOUND;
  const auto open = reinterpret_cast<OpenDirectory>(GetProcAddress(native, "NtOpenDirectoryObject"));
  const auto error = reinterpret_cast<NativeError>(GetProcAddress(native, "RtlNtStatusToDosError"));
  const auto compare = reinterpret_cast<Compare>(GetProcAddress(base, "CompareObjectHandles"));
  if (!open || !error || !compare) return ERROR_PROC_NOT_FOUND;
  std::wstring path = L"\\Sessions\\" + std::to_wstring(spec.desktop_session) + L"\\" + leaf;
  if (path.size() >= 128) return ERROR_INVALID_PARAMETER;
  UNICODE_STRING name{};
  name.Buffer = path.data();
  name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
  name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(wchar_t));
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.ObjectName = &name;
  attributes.Attributes = 0x00001000;  // OBJ_DONT_REPARSE
  HANDLE opened = nullptr;
  const NTSTATUS status = open(&opened, access, &attributes);
  Handle directory(opened == INVALID_HANDLE_VALUE ? nullptr : opened);
  if (status < 0) {
    const DWORD result = error(status);
    return result ? result : ERROR_GEN_FAILURE;
  }
  if (!directory) return ERROR_INVALID_HANDLE;
  // A typed open rejects a path/link/type substitution. The comparison also
  // rejects an otherwise valid handle for a different Windows session/root.
  if (!compare(expected, directory.get())) return ERROR_NOT_SAME_OBJECT;
  retained->reset(directory.release());
  return ERROR_SUCCESS;
}

bool AbsolutePath(std::wstring_view path) {
  if (path.size() < 4 || path[1] != L':' || path[2] != L'\\') return false;
  const wchar_t drive = path[0] | 0x20;
  if (drive < L'a' || drive > L'z') return false;
  return path.find_first_of(L"\":/", 2) == std::wstring_view::npos;
}

bool CompleteEnvironment(std::span<const wchar_t> environment) {
  if (environment.size() < 2 || environment.size() > 32767) return false;
  if (environment.front() == L'\0') return environment.size() == 2 && environment.back() == L'\0';
  for (size_t i = 1; i < environment.size(); ++i) {
    if (environment[i - 1] == L'\0' && environment[i] == L'\0') {
      return i + 1 == environment.size();
    }
  }
  return false;
}

DWORD SortedEnvironment(std::span<const wchar_t> input, std::vector<wchar_t>* output) {
  if (!CompleteEnvironment(input)) return ERROR_INVALID_PARAMETER;
  struct Entry { std::wstring_view name, value; };
  std::vector<Entry> entries;
  for (size_t start = 0; start < input.size() && input[start] != L'\0';) {
    const std::wstring_view remaining(input.data() + start, input.size() - start);
    const size_t end = remaining.find(L'\0');
    if (end == std::wstring_view::npos) return ERROR_INVALID_PARAMETER;
    const auto entry = remaining.substr(0, end);
    const size_t equal = entry.find(L'=');
    if (equal == 0 || equal == std::wstring_view::npos) return ERROR_INVALID_PARAMETER;
    entries.push_back({entry.substr(0, equal), entry});
    start += entry.size() + 1;
  }
  const auto compare = [](std::wstring_view left, std::wstring_view right) {
    const int order = CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE);
    if (!order) throw GetLastError();
    return order;
  };
  try {
    // Win32 requires case-insensitive Unicode name order, independent of the
    // current locale. Duplicate names must not shadow a trusted setting.
    std::sort(entries.begin(), entries.end(), [&](const Entry& left, const Entry& right) {
      return compare(left.name, right.name) == CSTR_LESS_THAN;
    });
    for (size_t i = 1; i < entries.size(); ++i) {
      if (compare(entries[i - 1].name, entries[i].name) == CSTR_EQUAL) return ERROR_INVALID_PARAMETER;
    }
  } catch (DWORD status) { return status ? status : ERROR_GEN_FAILURE; }
  output->reserve(input.size());
  for (const auto& entry : entries) {
    output->insert(output->end(), entry.value.begin(), entry.value.end());
    output->push_back(L'\0');
  }
  if (entries.empty()) output->push_back(L'\0');
  output->push_back(L'\0');
  return ERROR_SUCCESS;
}

bool ValidSpec(const AuthorityLaunchSpec& spec) {
  // Protected callers supply both independently pinned roots. An expected
  // leaf cannot turn an incomplete protected request into a legacy launch.
  if (static_cast<bool>(spec.session_objects) != static_cast<bool>(spec.app_container_objects) ||
      (spec.expected_package_objects && !spec.app_container_objects)) return false;
  if (!(spec.executable && spec.directory && AbsolutePath(spec.executable) &&
      AbsolutePath(spec.directory) && spec.arguments.size() <= 32767 &&
      spec.arguments.find(L'\0') == std::wstring_view::npos &&
      CompleteEnvironment(spec.environment) && spec.desktop_user &&
      IsValidSid(spec.desktop_user) && spec.desktop_session != 0 &&
      spec.capabilities.AppContainerSid && IsValidSid(spec.capabilities.AppContainerSid) &&
      spec.capabilities.CapabilityCount <= kMaximumCapabilities &&
      (!spec.capabilities.CapabilityCount || spec.capabilities.Capabilities) &&
      spec.capabilities.Reserved == 0)) return false;
  for (DWORD i = 0; i < spec.capabilities.CapabilityCount; ++i) {
    const auto& capability = spec.capabilities.Capabilities[i];
    if (!capability.Sid || !IsValidSid(capability.Sid) || capability.Attributes != SE_GROUP_ENABLED) return false;
    for (DWORD j = 0; j < i; ++j) {
      if (EqualSid(capability.Sid, spec.capabilities.Capabilities[j].Sid)) return false;
    }
  }
  return true;
}

DWORD MakeDescriptor(const wchar_t* policy, LocalMemory* output) {
  PSECURITY_DESCRIPTOR security = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(policy, SDDL_REVISION_1,
          &security, nullptr)) return GetLastError();
  output->reset(security);
  return ERROR_SUCCESS;
}

DWORD MakeGuestDefaultDescriptor(HANDLE token, PSID package, LocalMemory* output) {
  struct LogonGroup {
    TOKEN_GROUPS groups;
    BYTE sid[SECURITY_MAX_SID_SIZE];
  } token_logon{};
  DWORD bytes = 0;
  if (!GetTokenInformation(token, TokenLogonSid, &token_logon, sizeof(token_logon), &bytes))
    return GetLastError();
  const auto& logon = token_logon.groups.Groups[0];
  if (bytes < sizeof(TOKEN_GROUPS) || bytes > sizeof(token_logon) ||
      token_logon.groups.GroupCount != 1 ||
      (logon.Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID ||
      !logon.Sid || !IsValidSid(logon.Sid)) return ERROR_INVALID_SID;
  wchar_t* raw_logon = nullptr;
  if (!ConvertSidToStringSidW(logon.Sid, &raw_logon)) return GetLastError();
  const LocalMemory logon_name(raw_logon);
  wchar_t* raw_package = nullptr;
  if (!ConvertSidToStringSidW(package, &raw_package)) return GetLastError();
  const LocalMemory package_name(raw_package);
  // Windows initializes a lowbox using its real logon and package grants on
  // default-created objects, including rights to mutate their own objects.
  // OWNER RIGHTS preserves the native implicit READ_CONTROL grant. Explicit
  // authority descriptors protect the bootstrap, namespace and child objects.
  const std::wstring policy = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + raw_logon +
      L")(A;;RC;;;OW)(A;;GA;;;" + raw_package + L")";
  return MakeDescriptor(policy.c_str(), output);
}

PACL GetAcl(PSECURITY_DESCRIPTOR security) {
  PACL acl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  return GetSecurityDescriptorDacl(security, &present, &acl, &defaulted) && present ? acl : nullptr;
}

bool SameAcl(PACL expected, PACL actual) {
  if (!expected || !actual || !IsValidAcl(actual) || expected->AceCount != actual->AceCount) {
    return false;
  }
  for (DWORD i = 0; i < expected->AceCount; ++i) {
    void* left = nullptr;
    void* right = nullptr;
    if (!GetAce(expected, i, &left) || !GetAce(actual, i, &right)) return false;
    const WORD size = static_cast<ACE_HEADER*>(left)->AceSize;
    if (size != static_cast<ACE_HEADER*>(right)->AceSize || std::memcmp(left, right, size) != 0) {
      return false;
    }
  }
  return true;
}

DWORD VerifyObject(HANDLE object, PACL expected) {
  PSID owner = nullptr;
  PACL actual = nullptr;
  PSECURITY_DESCRIPTOR security = nullptr;
  const DWORD status = GetSecurityInfo(object, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &actual,
      nullptr, &security);
  if (status != ERROR_SUCCESS) return status;
  const LocalMemory release(security);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!owner || !IsWellKnownSid(owner, WinLocalSystemSid) || !SameAcl(expected, actual) ||
      !GetSecurityDescriptorControl(security, &control, &revision) || !(control & SE_DACL_PROTECTED)) {
    return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
}

// An explicit SACL requires SeSecurityPrivilege at object creation. Enable it
// only on a private SYSTEM impersonation token; never adjust the process token
// shared with other authority threads. The package wrapper runs outside this
// context; post-wrapper SACL finalization uses a separate scoped duplicate.
class PackageSaclPrivilege {
 public:
  explicit PackageSaclPrivilege(AuthorityNamespaceCreateResult* observed) : observed_(observed) {}
  ~PackageSaclPrivilege() { Restore(); }
  DWORD Enable() {
    if (!LocalSystemWorker()) return ERROR_ACCESS_DENIED;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw)) return GetLastError();
    const Handle process(raw);
    Identity source;
    if (!source.Read(raw) || source.stats.TokenType != TokenPrimary || source.contained ||
        !IsWellKnownSid(source.User(), WinLocalSystemSid)) return ERROR_ACCESS_DENIED;
    LocalMemory descriptor;
    // Like the private query context, let duplication choose the trusted
    // default owner. Specifying an owner here would require SeRestorePrivilege
    // on the shared process token. OWNER RIGHTS suppresses implicit mutation.
    static_assert(TOKEN_ALL_ACCESS == 0x000f01ff);
    DWORD status = MakeDescriptor(L"D:P(D;;WDWO;;;OW)(A;;0x000f01ff;;;SY)", &descriptor);
    if (status != ERROR_SUCCESS) return status;
    PACL expected_acl = GetAcl(descriptor.get());
    if (!expected_acl) return ERROR_INVALID_ACL;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
    raw = nullptr;
    if (!DuplicateTokenEx(process.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE | READ_CONTROL,
        &attributes, SecurityImpersonation, TokenImpersonation, &raw)) return GetLastError();
    token_.reset(raw);
    DWORD flags = 0;
    if (!GetHandleInformation(raw, &flags)) return GetLastError();
    if (flags & HANDLE_FLAG_INHERIT) return ERROR_ACCESS_DENIED;
    PSID owner = nullptr;
    PACL actual_acl = nullptr;
    PSECURITY_DESCRIPTOR actual_descriptor = nullptr;
    status = GetSecurityInfo(raw, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &actual_acl, nullptr, &actual_descriptor);
    const LocalMemory actual_owner(actual_descriptor);
    if (status != ERROR_SUCCESS) return status;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!owner || (!IsWellKnownSid(owner, WinLocalSystemSid) && !IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) ||
        !SameAcl(expected_acl, actual_acl) || !GetSecurityDescriptorControl(actual_descriptor, &control, &revision) ||
        !(control & SE_DACL_PROTECTED)) return ERROR_ACCESS_DENIED;
    Identity duplicate;
    if (!duplicate.Read(raw) || duplicate.stats.TokenType != TokenImpersonation || duplicate.contained ||
        duplicate.stats.ImpersonationLevel != SecurityImpersonation || !IsWellKnownSid(duplicate.User(), WinLocalSystemSid) ||
        duplicate.session != source.session || !SameLuid(duplicate.stats.AuthenticationId, source.stats.AuthenticationId) ||
        SameLuid(duplicate.stats.TokenId, source.stats.TokenId)) return ERROR_ACCESS_DENIED;
    token_id_ = duplicate.stats.TokenId;
    if (!LookupPrivilegeValueW(nullptr, L"SeSecurityPrivilege", &security_)) return GetLastError();
    status = ReadPrivilege(&before_);
    if (status != ERROR_SUCCESS) return status;
    observed_->privilege_before = before_;
    TOKEN_PRIVILEGES requested{};
    requested.PrivilegeCount = 1;
    requested.Privileges[0] = {security_, SE_PRIVILEGE_ENABLED};
    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted = AdjustTokenPrivileges(raw, FALSE, &requested, sizeof(previous_), &previous_, &bytes);
    const DWORD error = GetLastError();
    if (!adjusted) return error ? error : ERROR_GEN_FAILURE;
    adjusted_ = true;
    observed_->privilege_changes = previous_.PrivilegeCount;
    // TRUE alone does not establish assignment. Restore even on partial error.
    if (error != ERROR_SUCCESS) return error;
    if (bytes > sizeof(previous_) || previous_.PrivilegeCount > 1) return ERROR_INVALID_DATA;
    DWORD enabled = 0;
    status = ReadPrivilege(&enabled);
    observed_->privilege_enabled = enabled;
    if (status != ERROR_SUCCESS) return status;
    return (enabled & SE_PRIVILEGE_ENABLED) ? ERROR_SUCCESS : ERROR_PRIVILEGE_NOT_HELD;
  }
  DWORD Bind() {
    if (!SetThreadToken(nullptr, token_.get())) return GetLastError();
    bound_ = true;
    HANDLE raw = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) return GetLastError();
    const Handle current(raw);
    TOKEN_STATISTICS statistics{};
    if (!Query(raw, TokenStatistics, &statistics)) return GetLastError();
    return statistics.TokenType == TokenImpersonation && statistics.ImpersonationLevel == SecurityImpersonation &&
        SameLuid(statistics.TokenId, token_id_) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
  }
  DWORD Restore() {
    if (restored_) return restore_status_;
    restored_ = true;
    DWORD privilege_status = ERROR_SUCCESS;
    if (adjusted_) {
      adjusted_ = false;
      SetLastError(ERROR_SUCCESS);
      const BOOL restored = AdjustTokenPrivileges(token_.get(), FALSE, &previous_, 0, nullptr, nullptr);
      privilege_status = GetLastError();
      if (!restored && !privilege_status) privilege_status = ERROR_GEN_FAILURE;
      DWORD actual = 0;
      const DWORD queried = ReadPrivilege(&actual);
      observed_->privilege_restored = actual;
      if (privilege_status == ERROR_SUCCESS) {
        privilege_status = queried ? queried : (actual == before_ ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
      }
    }
    observed_->privilege_restore_status = privilege_status;
    if (bound_) {
      if (!RevertToSelf()) {
        observed_->context_restore_status = GetLastError();
        std::abort();  // Never return to the service under a suspect context.
      }
      bound_ = false;
    }
    HANDLE raw = nullptr;
    const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw);
    const DWORD error = GetLastError();
    const Handle unexpected(raw);
    observed_->context_restore_status = !opened && error == ERROR_NO_TOKEN ? ERROR_SUCCESS :
        (opened ? ERROR_ACCESS_DENIED : (error ? error : ERROR_GEN_FAILURE));
    if (observed_->context_restore_status != ERROR_SUCCESS) std::abort();
    restore_status_ = privilege_status;
    return restore_status_;
  }
 private:
  static bool SameLuid(const LUID& left, const LUID& right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
  }
  DWORD ReadPrivilege(DWORD* attributes) const {
    struct Privileges { DWORD count; LUID_AND_ATTRIBUTES entries[128]; } info{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token_.get(), TokenPrivileges, &info, sizeof(info), &bytes)) return GetLastError();
    if (bytes < offsetof(Privileges, entries) || bytes > sizeof(info) || info.count > std::size(info.entries) ||
        info.count > (bytes - offsetof(Privileges, entries)) / sizeof(LUID_AND_ATTRIBUTES)) return ERROR_INVALID_DATA;
    for (DWORD i = 0; i < info.count; ++i) {
      if (SameLuid(info.entries[i].Luid, security_)) {
        *attributes = info.entries[i].Attributes;
        return ERROR_SUCCESS;
      }
    }
    return ERROR_NOT_ALL_ASSIGNED;
  }
  AuthorityNamespaceCreateResult* observed_;
  Handle token_;
  LUID security_{}, token_id_{};
  TOKEN_PRIVILEGES previous_{};
  DWORD before_ = 0, restore_status_ = ERROR_SUCCESS;
  bool adjusted_ = false, bound_ = false, restored_ = false;
};

// A protected package root is authority-created and immutable to lowboxes.
// The private non-lowbox wrapper needs DIRECTORY_ALL_ACCESS on this leaf, but
// the package only needs to query/traverse and create objects/subdirectories.
// Inherit-only GA entries give those children their normal object-type rights
// without giving a lowbox DELETE/WRITE_DAC/WRITE_OWNER on the package root.
// Children need both the private-user and package grants for lowbox reopening.
// SYSTEM and administrators able to act as SYSTEM remain trusted. A prior
// private/desktop-owned leaf is never repaired into this trusted invariant:
// its creator could still hold a mutation handle granted before such repair.
class PackageNamespace {
 public:
  static constexpr ACCESS_MASK kRead = READ_CONTROL | 0x0001 | 0x0002;
  static constexpr ACCESS_MASK kPipeRead = READ_CONTROL | SYNCHRONIZE;

  DWORD Initialize(const AuthorityLaunchSpec& spec, HANDLE base) {
    const HMODULE native = GetModuleHandleW(L"ntdll.dll");
    const HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    if (!native || !kernel) return ERROR_MOD_NOT_FOUND;
    open_ = reinterpret_cast<Open>(GetProcAddress(native, "NtOpenDirectoryObject"));
    create_ = reinterpret_cast<Create>(GetProcAddress(native, "NtCreateDirectoryObjectEx"));
    create_file_ = reinterpret_cast<CreateFileFn>(GetProcAddress(native, "NtCreateFile"));
    query_ = reinterpret_cast<QuerySecurity>(GetProcAddress(native, "NtQuerySecurityObject"));
    set_ = reinterpret_cast<SetSecurity>(GetProcAddress(native, "NtSetSecurityObject"));
    error_ = reinterpret_cast<NativeError>(GetProcAddress(native, "RtlNtStatusToDosError"));
    compare_ = reinterpret_cast<Compare>(GetProcAddress(kernel, "CompareObjectHandles"));
    if (!open_ || !create_ || !create_file_ || !query_ || !set_ || !error_ || !compare_) return ERROR_PROC_NOT_FOUND;
    wchar_t* package = nullptr;
    if (!ConvertSidToStringSidW(spec.capabilities.AppContainerSid, &package)) return GetLastError();
    const LocalMemory package_owner(package);
    wchar_t* user = nullptr;
    if (!ConvertSidToStringSidW(spec.desktop_user, &user)) return GetLastError();
    const LocalMemory user_owner(user);
    name_ = package;
    if (name_.empty() || name_.size() > 184 || name_.find_first_of(L"\\/") != std::wstring::npos) {
      return ERROR_INVALID_SID;
    }
    const std::wstring policy = std::wstring(L"O:SYG:SYD:P(A;;0x000f000f;;;SY)(A;OICIIO;GA;;;SY)") +
        L"(A;;0x000f000f;;;" + user + L")(A;OICIIO;GA;;;" + user + L")(A;;0x0002000f;;;" + package +
        L")(A;OICIIO;GA;;;" + package + L")S:P(ML;;NW;;;LW)";
    // Match the NamedPipe AppContainer leaf so CreateAppContainerToken can open
    // it under a medium-integrity restricted token even when the desktop session's
    // NamedPipe AppContainerNamedObjects parent denies child creation.
    const std::wstring pipe_policy = std::wstring(L"O:SYG:SYD:P(A;;0x001f01ff;;;SY)(A;OICIIO;GA;;;SY)") +
        L"(A;;0x001f01ff;;;" + user + L")(A;OICIIO;GA;;;" + user + L")(A;;0x001201ff;;;" + package +
        L")(A;OICIIO;GA;;;" + package + L")S:P(ML;;NW;;;LW)";
    pipe_name_ = L"\\Device\\NamedPipe\\Sessions\\" + std::to_wstring(spec.desktop_session) +
        L"\\AppContainerNamedObjects\\" + name_;
    DWORD status = MakeDescriptor(policy.c_str(), &policy_);
    if (status == ERROR_SUCCESS) status = MakeDescriptor(pipe_policy.c_str(), &pipe_policy_);
    if (status != ERROR_SUCCESS) return status;
    expected_dacl_ = GetAcl(policy_.get());
    BOOL present = FALSE, defaulted = FALSE;
    if (!expected_dacl_ || !GetSecurityDescriptorSacl(policy_.get(), &present, &expected_label_, &defaulted) ||
        !present || !expected_label_) return ERROR_INVALID_SECURITY_DESCR;
    expected_pipe_dacl_ = GetAcl(pipe_policy_.get());
    if (!expected_pipe_dacl_ || !GetSecurityDescriptorSacl(pipe_policy_.get(), &present, &expected_pipe_label_, &defaulted) ||
        !present || !expected_pipe_label_) return ERROR_INVALID_SECURITY_DESCR;
    shadow_ = base;
    return ERROR_SUCCESS;
  }

  DWORD Prepare(const AuthorityLaunchSpec& spec, HANDLE root, Handle* retained,
                Handle* retained_pipe, const char** startup_stage) {
    const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
    struct Result {
      AuthorityNamespaceCreateResult value;
      AuthorityNamespaceCreateResult* output;
      ~Result() { if (output) *output = value; }
    } observed{{}, spec.namespace_create_result};
    stage("lowbox.verify-package-namespace");
    NTSTATUS native = 0;
    DWORD status = OpenLeaf(root, retained, &native);
    const bool create_directory = status != ERROR_SUCCESS;
    if (create_directory) {
      if (!Missing(native)) return status;
      if (spec.expected_package_objects) return ERROR_NOT_SAME_OBJECT;
    } else {
      // Reject an untrusted normal namespace before creating its pipe sibling.
      if (spec.expected_package_objects) {
        status = SamePin(spec.expected_package_objects, retained->get());
        if (status != ERROR_SUCCESS) return status;
      }
      status = VerifyPolicy(retained->get(), spec.namespace_create_result ? &observed.value.directory_policy : nullptr);
      if (status != ERROR_SUCCESS) return status;
    }
    stage("lowbox.open-package-pipe-namespace");
    NTSTATUS pipe_native = 0;
    status = OpenPipe(retained_pipe, &pipe_native);
    observed.value.pipe_open_status = status;
    const bool create_pipe = status != ERROR_SUCCESS;
    if (create_pipe && !Missing(pipe_native)) return status;
    if (create_directory || create_pipe) {
      Handle create_root;
      if (create_directory) {
        // A temporary SYSTEM-only create-subdirectory handle is independently
        // compared to the same root. No parent ACL is changed.
        stage("lowbox.open-package-create-root");
        status = VerifySessionObjects(spec, SessionObjectRoot::kAppContainer, &create_root, kRead | 0x0008);
        observed.value.root_open_status = status;
        if (status != ERROR_SUCCESS) return status;
      }
      PackageSaclPrivilege privilege(&observed.value);
      stage("lowbox.enable-package-sacl-privilege");
      status = privilege.Enable();
      observed.value.privilege_status = status;
      if (status != ERROR_SUCCESS) return status;
      stage("lowbox.bind-package-create-context");
      status = privilege.Bind();
      observed.value.bind_status = status;
      if (status != ERROR_SUCCESS) return status;
      Handle created;
      if (create_directory) {
        UNICODE_STRING name = Name();
        OBJECT_ATTRIBUTES attributes = Attributes(create_root.get(), &name);
        attributes.SecurityDescriptor = policy_.get();
        HANDLE raw = nullptr;
        // The five-argument native ABI takes a QUERY|TRAVERSE shadow Directory.
        // Option 1 is observed on both supported Windows wrappers. No OPENIF.
        stage("lowbox.create-package-namespace");
        observed.value.native_called = 1;
        native = create_(&raw, kRead, &attributes, shadow_, 1);
        observed.value.native_status = native;
        created.reset(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
      } else {
        native = 0;
      }
      Handle pipe_created;
      if (create_pipe && (native == 0 || native == kCollision)) {
        UNICODE_STRING name = PipeName();
        OBJECT_ATTRIBUTES attributes = Attributes(nullptr, &name);
        attributes.Attributes |= 0x00000040;  // OBJ_CASE_INSENSITIVE
        attributes.SecurityDescriptor = pipe_policy_.get();
        IO_STATUS_BLOCK iosb{};
        HANDLE raw = nullptr;
        stage("lowbox.create-package-pipe-namespace");
        observed.value.pipe_create_called = 1;
        // FILE_CREATE detects a race. An existing directory is reopened and
        // authenticated below; its descriptor is never silently repaired.
        pipe_native = create_file_(&raw, 0x001f0006, &attributes, &iosb, nullptr,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            2 /* FILE_CREATE */, 1 /* FILE_DIRECTORY_FILE */, nullptr, 0);
        observed.value.pipe_create_status = pipe_native;
        pipe_created.reset(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
      }
      stage("lowbox.restore-package-create-context");
      const DWORD restored = privilege.Restore();
      if (native < 0 && native != kCollision) {
        stage("lowbox.create-package-namespace");
        return Error(native);
      }
      if (create_pipe && pipe_native < 0 && pipe_native != kCollision) {
        stage("lowbox.create-package-pipe-namespace");
        return Error(pipe_native);
      }
      if (restored != ERROR_SUCCESS) return restored;
      if (create_directory) {
        stage("lowbox.create-package-namespace");
        if (native == 0) {
          if (!created) return ERROR_INVALID_HANDLE;
          retained->reset(created.release());
        } else if (native == kCollision) {
          status = OpenLeaf(root, retained);
          if (status != ERROR_SUCCESS) return status;
        } else {
          return ERROR_INVALID_DATA;
        }
      }
      if (create_pipe) {
        stage("lowbox.open-package-pipe-namespace");
        if (pipe_native == 0) {
          if (!pipe_created) return ERROR_INVALID_HANDLE;
          HANDLE read_only = nullptr;
          if (!DuplicateHandle(GetCurrentProcess(), pipe_created.get(), GetCurrentProcess(),
              &read_only, kPipeRead, FALSE, 0)) return GetLastError();
          retained_pipe->reset(read_only);
        } else if (pipe_native == kCollision) {
          status = OpenPipe(retained_pipe);
          if (status != ERROR_SUCCESS) return status;
        } else {
          return ERROR_INVALID_DATA;
        }
        // The reduced duplicate retains the NPFS directory before the creation
        // handle closes. No mutation-capable or inheritable handle survives.
      }
    }
    stage("lowbox.verify-package-namespace");
    if (spec.expected_package_objects) {
      status = SamePin(spec.expected_package_objects, retained->get());
      if (status != ERROR_SUCCESS) return status;
    }
    status = VerifyPolicy(retained->get(), spec.namespace_create_result ? &observed.value.directory_policy : nullptr);
    if (status != ERROR_SUCCESS) return status;
    stage("lowbox.verify-package-pipe-namespace");
    status = VerifyPipePolicy(retained_pipe->get(), spec.namespace_create_result ? &observed.value.pipe_policy : nullptr);
    observed.value.pipe_policy_status = status;
    if (status == ERROR_SUCCESS) status = Recheck(root, retained->get(), retained_pipe->get(),
        spec.namespace_create_result ? &observed.value.recheck : nullptr);
    prepared_ = status == ERROR_SUCCESS;
    return status;
  }

  DWORD SealAfterLowbox(HANDLE root, HANDLE expected, AuthorityNamespaceSealResult* output) {
    using Stage = AuthorityNamespaceSealStage;
    AuthorityNamespaceCreateResult privilege_observed;
    struct Result {
      AuthorityNamespaceSealResult value;
      AuthorityNamespaceSealResult* output;
      const AuthorityNamespaceCreateResult* privilege;
      ~Result() {
        value.privilege_before = privilege->privilege_before;
        value.privilege_enabled = privilege->privilege_enabled;
        value.privilege_restored = privilege->privilege_restored;
        value.privilege_changes = privilege->privilege_changes;
        value.privilege_restore_status = privilege->privilege_restore_status;
        value.context_restore_status = privilege->context_restore_status;
        if (output) *output = value;
      }
    } observed{{}, output, &privilege_observed};
    const auto stage = [&](Stage value) { observed.value.stage = value; };
    stage(Stage::kPrepared);
    if (!prepared_ || resealed_ || !have_control_) return observed.value.status = ERROR_ACCESS_DENIED;
    resealed_ = true;

    // The verified wrapper resets SACL protection/auto-inheritance on the same
    // leaf while setting its low label. Only that exact intermediate state is
    // eligible here. Ordinary preparation/reuse/collision admission stays strict.
    // Session serializes launches for a package across this entire transition.
    PackageSaclPrivilege privilege(&privilege_observed);
    Handle writable;
    const DWORD operation = [&]() -> DWORD {
      stage(Stage::kPrivilege);
      DWORD status = privilege.Enable();
      observed.value.privilege_status = status;
      if (status != ERROR_SUCCESS) return status;
      stage(Stage::kBind);
      status = privilege.Bind();
      observed.value.bind_status = status;
      if (status != ERROR_SUCCESS) return status;
      stage(Stage::kOpen);
      status = OpenLeaf(root, &writable, &observed.value.open_status, READ_CONTROL | ACCESS_SYSTEM_SECURITY);
      if (status != ERROR_SUCCESS) return status;
      stage(Stage::kSamePin);
      status = SamePin(expected, writable.get());
      if (status != ERROR_SUCCESS) return status;
      stage(Stage::kFullPolicy);
      status = VerifyPolicy(writable.get(), expected_dacl_, expected_label_, &have_control_, &control_,
          &observed.value.before_policy, PolicyRead::kWrapperSacl);
      if (status != ERROR_SUCCESS) return status;

      // The privileged full read above rejects every extra SACL ACE before any
      // replacement. A label-only read cannot establish that no audit entry is
      // being erased. Restore only this policy's exact single low-MIC ACL and
      // original SACL control bits; owner, group and DACL are never written.
      stage(Stage::kDescriptor);
      SECURITY_DESCRIPTOR descriptor{};
      constexpr WORD sacl_control = SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED | SE_SACL_AUTO_INHERIT_REQ;
      WORD requested_control = static_cast<WORD>(control_ & sacl_control);
      // MS-DTYP specifies that SC+SI input preserves SI while consuming SC.
      // Request that transition only for a previously auto-inherited SACL;
      // strict readback must still reproduce the original saved control.
      if (control_ & SE_SACL_AUTO_INHERITED) requested_control |= SE_SACL_AUTO_INHERIT_REQ;
      if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
          !SetSecurityDescriptorSacl(&descriptor, TRUE, expected_label_, FALSE) ||
          !SetSecurityDescriptorControl(&descriptor, sacl_control, requested_control)) return GetLastError();
      stage(Stage::kWrite);
      observed.value.write_called = 1;
      const NTSTATUS written = set_(writable.get(), SACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION,
          &descriptor);
      observed.value.write_status = written;
      if (written < 0) return Error(written);
      stage(Stage::kReadback);
      return VerifyPolicy(writable.get(), expected_dacl_, expected_label_, &have_control_, &control_,
          &observed.value.after_policy, PolicyRead::kCompleteSacl);
    }();

    // Close the only temporary SACL-capable handle before returning to the
    // ordinary authority context. Preserve the primary, close and restoration
    // failures separately; successful launch requires every one to succeed.
    DWORD close_status = ERROR_SUCCESS;
    if (writable) {
      observed.value.close_called = 1;
      if (CloseHandle(writable.get())) static_cast<void>(writable.release());
      else close_status = GetLastError();
    }
    observed.value.close_status = close_status;
    const DWORD restored = privilege.Restore();
    if (operation != ERROR_SUCCESS) return observed.value.status = operation;
    if (close_status != ERROR_SUCCESS) { stage(Stage::kClose); return observed.value.status = close_status; }
    if (restored != ERROR_SUCCESS) { stage(Stage::kRestore); return observed.value.status = restored; }
    stage(Stage::kComplete);
    return observed.value.status = ERROR_SUCCESS;
  }

  DWORD Recheck(HANDLE root, HANDLE expected, HANDLE pipe,
                AuthorityNamespaceRecheckResult* observed = nullptr) {
    using Stage = AuthorityNamespaceRecheckStage;
    if (observed) {
      const DWORD attempts = observed->attempts + 1;
      *observed = {};
      observed->attempts = attempts;
    }
    const auto stage = [observed](Stage value) { if (observed) observed->stage = value; };
    const auto finish = [observed](DWORD status) {
      if (observed) observed->status = status;
      return status;
    };
    Handle fresh;
    stage(Stage::kLeafOpen);
    DWORD status = OpenLeaf(root, &fresh, observed ? &observed->leaf_open_status : nullptr);
    if (status != ERROR_SUCCESS) return finish(status);
    stage(Stage::kSamePin);
    status = SamePin(expected, fresh.get());
    if (status != ERROR_SUCCESS) return finish(status);
    stage(Stage::kDirectoryPolicy);
    status = VerifyPolicy(fresh.get(), observed ? &observed->directory_policy : nullptr);
    if (status != ERROR_SUCCESS) return finish(status);
    stage(Stage::kPipePolicy);
    status = VerifyPipePolicy(pipe, observed ? &observed->pipe_policy : nullptr);
    if (status == ERROR_SUCCESS) stage(Stage::kComplete);
    return finish(status);
  }

 private:
  using Open = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
  using Create = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, ULONG);
  using CreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
      PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  using QuerySecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG);
  using SetSecurity = NTSTATUS(NTAPI*)(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
  using NativeError = ULONG(NTAPI*)(NTSTATUS);
  using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
  static constexpr DWORD kSecurityLimit = 16384;
  static constexpr NTSTATUS kCollision = static_cast<NTSTATUS>(0xc0000035);
  static bool Missing(NTSTATUS status) {
    return status == static_cast<NTSTATUS>(0xc0000034) || status == static_cast<NTSTATUS>(0xc000003a);
  }
  Open open_ = nullptr;
  Create create_ = nullptr;
  CreateFileFn create_file_ = nullptr;
  QuerySecurity query_ = nullptr;
  SetSecurity set_ = nullptr;
  NativeError error_ = nullptr;
  Compare compare_ = nullptr;
  HANDLE shadow_ = nullptr;
  std::wstring name_, pipe_name_;
  LocalMemory policy_, pipe_policy_;
  PACL expected_dacl_ = nullptr, expected_label_ = nullptr;
  PACL expected_pipe_dacl_ = nullptr, expected_pipe_label_ = nullptr;
  bool have_control_ = false, have_pipe_control_ = false;
  bool prepared_ = false, resealed_ = false;
  SECURITY_DESCRIPTOR_CONTROL control_ = 0, pipe_control_ = 0;
  enum class PolicyRead { kLabel, kCompleteSacl, kWrapperSacl };

  DWORD Error(NTSTATUS status) const {
    const DWORD result = error_(status);
    return result ? result : ERROR_GEN_FAILURE;
  }
  UNICODE_STRING Name() {
    UNICODE_STRING value{};
    value.Buffer = name_.data();
    value.Length = static_cast<USHORT>(name_.size() * sizeof(wchar_t));
    value.MaximumLength = static_cast<USHORT>(value.Length + sizeof(wchar_t));
    return value;
  }
  UNICODE_STRING PipeName() {
    UNICODE_STRING value{};
    value.Buffer = pipe_name_.data();
    value.Length = static_cast<USHORT>(pipe_name_.size() * sizeof(wchar_t));
    value.MaximumLength = static_cast<USHORT>(value.Length + sizeof(wchar_t));
    return value;
  }
  static OBJECT_ATTRIBUTES Attributes(HANDLE root, UNICODE_STRING* name) {
    OBJECT_ATTRIBUTES value{};
    value.Length = sizeof(value);
    value.RootDirectory = root;
    value.ObjectName = name;
    value.Attributes = 0x00001000;  // OBJ_DONT_REPARSE; no OPENIF/INHERIT/PERMANENT.
    return value;
  }
  DWORD OpenLeaf(HANDLE root, Handle* result, NTSTATUS* native_status = nullptr, ACCESS_MASK access = kRead) {
    UNICODE_STRING name = Name();
    OBJECT_ATTRIBUTES attributes = Attributes(root, &name);
    HANDLE raw = nullptr;
    const NTSTATUS status = open_(&raw, access, &attributes);
    Handle opened(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
    if (native_status) *native_status = status;
    if (status < 0) return Error(status);
    if (!opened) return ERROR_INVALID_HANDLE;
    DWORD flags = 0;
    if (!GetHandleInformation(opened.get(), &flags)) return GetLastError();
    if (flags & HANDLE_FLAG_INHERIT) return ERROR_ACCESS_DENIED;
    result->reset(opened.release());
    return ERROR_SUCCESS;
  }
  // KernelBase requests DELETE as part of its full-access directory open.
  // Sharing permits that trusted wrapper request; the exact package ACL still
  // denies DELETE/WRITE_DAC/WRITE_OWNER to every lowbox subject.
  DWORD OpenPipe(Handle* result, NTSTATUS* native_status = nullptr) {
    UNICODE_STRING name = PipeName();
    OBJECT_ATTRIBUTES attributes = Attributes(nullptr, &name);
    attributes.Attributes |= 0x00000040;  // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK iosb{};
    HANDLE raw = nullptr;
    const NTSTATUS status = create_file_(&raw, kPipeRead, &attributes, &iosb, nullptr,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        1 /* FILE_OPEN */, 1 /* FILE_DIRECTORY_FILE */, nullptr, 0);
    Handle opened(raw == INVALID_HANDLE_VALUE ? nullptr : raw);
    if (native_status) *native_status = status;
    if (status < 0) return Error(status);
    if (!opened) return ERROR_INVALID_HANDLE;
    DWORD flags = 0;
    if (!GetHandleInformation(opened.get(), &flags)) return GetLastError();
    if (flags & HANDLE_FLAG_INHERIT) return ERROR_ACCESS_DENIED;
    result->reset(opened.release());
    return ERROR_SUCCESS;
  }
  DWORD SamePin(HANDLE expected, HANDLE actual) const {
    DWORD flags = 0;
    if (!GetHandleInformation(expected, &flags)) return GetLastError();
    if (flags & HANDLE_FLAG_INHERIT) return ERROR_ACCESS_DENIED;
    return compare_(expected, actual) ? ERROR_SUCCESS : ERROR_NOT_SAME_OBJECT;
  }
  static bool Within(const void* value, size_t length, const BYTE* begin, DWORD bytes) {
    const auto first = reinterpret_cast<ULONG_PTR>(begin), address = reinterpret_cast<ULONG_PTR>(value);
    return value && address >= first && address - first <= bytes && length <= bytes - (address - first);
  }
  static bool SidWithin(PSID sid, const BYTE* begin, DWORD bytes) {
    if (!Within(sid, 8, begin, bytes)) return false;
    const DWORD length = 8 + static_cast<DWORD>(*GetSidSubAuthorityCount(sid)) * static_cast<DWORD>(sizeof(DWORD));
    return Within(sid, length, begin, bytes) && IsValidSid(sid) && GetLengthSid(sid) == length;
  }
  static bool AclWithin(PACL acl, const BYTE* begin, DWORD bytes) {
    return Within(acl, sizeof(ACL), begin, bytes) && acl->AclSize >= sizeof(ACL) &&
        acl->AceCount <= 16 && Within(acl, acl->AclSize, begin, bytes) && IsValidAcl(acl);
  }
  DWORD VerifyPolicy(HANDLE directory, AuthorityNamespacePolicyResult* observed = nullptr) {
    return VerifyPolicy(directory, expected_dacl_, expected_label_, &have_control_, &control_, observed);
  }
  DWORD VerifyPipePolicy(HANDLE pipe, AuthorityNamespacePolicyResult* observed = nullptr) {
    DWORD flags = 0;
    if (!pipe || !GetHandleInformation(pipe, &flags)) return ERROR_INVALID_HANDLE;
    if (flags & HANDLE_FLAG_INHERIT) return ERROR_ACCESS_DENIED;
    return VerifyPolicy(pipe, expected_pipe_dacl_, expected_pipe_label_, &have_pipe_control_, &pipe_control_, observed);
  }
  DWORD VerifyPolicy(HANDLE directory, PACL expected_dacl, PACL expected_label,
                     bool* have_control, SECURITY_DESCRIPTOR_CONTROL* saved_control,
                     AuthorityNamespacePolicyResult* observed = nullptr, PolicyRead read = PolicyRead::kLabel) {
    const auto stage = [observed](AuthorityNamespacePolicyStage value) {
      if (observed) observed->stage = value;
    };
    const SECURITY_INFORMATION information = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION |
        (read == PolicyRead::kLabel ? 0 : SACL_SECURITY_INFORMATION);
    // NPFS secure prefixes return a valid descriptor but zero LengthNeeded on
    // successful queries on Windows 2022 and 2025. Obtain a bounded allocation
    // from the size probe; never infer a missing descriptor or an implicit label.
    ULONG allocation = 0;
    stage(AuthorityNamespacePolicyStage::kSizeQuery);
    const NTSTATUS sized = query_(directory, information, nullptr, 0, &allocation);
    if (observed) {
      observed->size_query_status = sized;
      observed->allocation_bytes = allocation;
      observed->requested_information = information;
    }
    if (sized != static_cast<NTSTATUS>(0xc0000023) /* STATUS_BUFFER_TOO_SMALL */) {
      return sized < 0 ? Error(sized) : ERROR_INVALID_SECURITY_DESCR;
    }
    stage(AuthorityNamespacePolicyStage::kLength);
    if (allocation < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || allocation > kSecurityLimit) return ERROR_INVALID_SECURITY_DESCR;
    std::vector<DWORD> storage((allocation + sizeof(DWORD) - 1) / sizeof(DWORD));
    ULONG returned = 0;
    stage(AuthorityNamespacePolicyStage::kQuery);
    const NTSTATUS status = query_(directory, information, storage.data(), allocation, &returned);
    if (observed) {
      observed->query_status = status;
      observed->returned_bytes = returned;
      if (status >= 0) {
        static_assert(sizeof(observed->descriptor) <= kSecurityLimit);
        observed->captured_bytes = std::min<DWORD>(allocation, static_cast<DWORD>(sizeof(observed->descriptor)));
        std::memcpy(observed->descriptor, storage.data(), observed->captured_bytes);
      }
    }
    if (status < 0) return Error(status);
    stage(AuthorityNamespacePolicyStage::kLength);
    if (returned && (returned < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || returned > allocation)) return ERROR_INVALID_SECURITY_DESCR;
    const DWORD bytes = returned ? returned : allocation;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr, label = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    stage(AuthorityNamespacePolicyStage::kControl);
    if (!GetSecurityDescriptorControl(storage.data(), &control, &revision) ||
        revision != SECURITY_DESCRIPTOR_REVISION || !(control & SE_SELF_RELATIVE)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kOwner);
    if (!GetSecurityDescriptorOwner(storage.data(), &owner, &defaulted)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kGroup);
    if (!GetSecurityDescriptorGroup(storage.data(), &group, &defaulted)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kDacl);
    if (!GetSecurityDescriptorDacl(storage.data(), &present, &dacl, &defaulted) || !present || !dacl) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kLabel);
    if (!GetSecurityDescriptorSacl(storage.data(), &present, &label, &defaulted) || !present || !label) return ERROR_INVALID_SECURITY_DESCR;
    const auto* begin = reinterpret_cast<const BYTE*>(storage.data());
    stage(AuthorityNamespacePolicyStage::kOwnerBounds);
    if (!SidWithin(owner, begin, bytes)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kGroupBounds);
    if (!SidWithin(group, begin, bytes)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kDaclBounds);
    if (!AclWithin(dacl, begin, bytes)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kLabelBounds);
    if (!AclWithin(label, begin, bytes)) return ERROR_INVALID_SECURITY_DESCR;
    stage(AuthorityNamespacePolicyStage::kDescriptor);
    if (!IsValidSecurityDescriptor(storage.data())) return ERROR_INVALID_SECURITY_DESCR;
    constexpr WORD protected_lists = SE_DACL_PROTECTED | SE_SACL_PROTECTED;
    constexpr WORD wrapper_reset = SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED;
    stage(AuthorityNamespacePolicyStage::kPolicy);
    const bool strict_control = (control & protected_lists) == protected_lists &&
        (!*have_control || control == *saved_control);
    const bool wrapper_control = read == PolicyRead::kWrapperSacl && *have_control &&
        control == static_cast<WORD>(*saved_control & ~wrapper_reset);
    if (!IsWellKnownSid(owner, WinLocalSystemSid) || !IsWellKnownSid(group, WinLocalSystemSid) ||
        (!strict_control && !wrapper_control) || dacl->AclRevision != expected_dacl->AclRevision ||
        label->AclRevision != expected_label->AclRevision || !SameAcl(expected_dacl, dacl) ||
        !SameAcl(expected_label, label)) return ERROR_ACCESS_DENIED;
    if (read != PolicyRead::kWrapperSacl) {
      *saved_control = control;
      *have_control = true;
    }
    stage(AuthorityNamespacePolicyStage::kComplete);
    return ERROR_SUCCESS;
  }
};

bool HasOnlyTraversePrivilege(HANDLE token) {
  alignas(TOKEN_PRIVILEGES) BYTE info[128]{};
  if (!Query(token, TokenPrivileges, info, sizeof(info))) return false;
  const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(info);
  if (privileges->PrivilegeCount == 0) return true;
  LUID traverse{};
  if (privileges->PrivilegeCount != 1 ||
      !LookupPrivilegeValueW(nullptr, L"SeChangeNotifyPrivilege", &traverse)) return false;
  const LUID& only = privileges->Privileges[0].Luid;
  return only.LowPart == traverse.LowPart && only.HighPart == traverse.HighPart;
}

DWORD CreatePrivateQueryContext(HANDLE primary, const Identity& expected, const AuthorityLaunchSpec& spec,
                                PACL default_acl, Handle* result, TOKEN_STATISTICS* statistics,
                                const char** startup_stage) {
  const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
  stage("lowbox.impersonation-policy");
  wchar_t* raw_sid = nullptr;
  if (!ConvertSidToStringSidW(spec.desktop_user, &raw_sid)) return GetLastError();
  const LocalMemory sid_owner(raw_sid);
  // The private identity may query this disposable thread-context token. All
  // other token rights remain with SYSTEM; neither token nor authority handles
  // are handed to the guest. Use token-specific masks for object ACL readback,
  // independently of the unchanged TokenDefaultDacl for newly created objects.
  static_assert(TOKEN_ALL_ACCESS == 0x000f01ff && TOKEN_QUERY == 0x00000008);
  const std::wstring policy = std::wstring(L"D:P(D;;WDWO;;;OW)(A;;0x000f01ff;;;SY)(A;;0x00000008;;;") + raw_sid + L")";
  LocalMemory descriptor;
  DWORD status = MakeDescriptor(policy.c_str(), &descriptor);
  if (status != ERROR_SUCCESS) return status;
  PACL expected_acl = GetAcl(descriptor.get());
  if (!expected_acl) return ERROR_INVALID_ACL;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  HANDLE raw = nullptr;
  stage("lowbox.create-impersonation-context");
  if (!DuplicateTokenEx(primary, TOKEN_QUERY | TOKEN_IMPERSONATE | READ_CONTROL, &security,
      SecurityImpersonation, TokenImpersonation, &raw)) return GetLastError();
  Handle token(raw);
  Identity identity;
  SECURITY_IMPERSONATION_LEVEL level{};
  stage("lowbox.impersonation-identity");
  if (!identity.Read(raw)) return ERROR_INVALID_DATA;
  if (!Query(raw, TokenImpersonationLevel, &level)) return GetLastError();
  if (!identity.MatchesDesktop(spec, TokenImpersonation) || level != SecurityImpersonation || identity.contained ||
      !IsWellKnownSid(identity.Integrity(), WinMediumLabelSid) || !HasOnlyTraversePrivilege(raw) ||
      identity.stats.AuthenticationId.HighPart != expected.stats.AuthenticationId.HighPart ||
      identity.stats.AuthenticationId.LowPart != expected.stats.AuthenticationId.LowPart) return ERROR_ACCESS_DENIED;
  stage("lowbox.impersonation-object-acl");
  PSID owner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR object_descriptor = nullptr;
  status = GetSecurityInfo(raw, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &acl, nullptr, &object_descriptor);
  const LocalMemory object_owner(object_descriptor);
  if (status != ERROR_SUCCESS) return status;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  // Duplication under SYSTEM can default the token object owner to its
  // Administrators owner SID. This exception applies only to this token;
  // VerifyObject still requires SYSTEM for every actual process and thread.
  if (!owner || (!IsWellKnownSid(owner, WinLocalSystemSid) && !IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) ||
      !SameAcl(expected_acl, acl) || !GetSecurityDescriptorControl(object_descriptor, &control, &revision) ||
      !(control & SE_DACL_PROTECTED)) return ERROR_ACCESS_DENIED;
  stage("lowbox.impersonation-default-acl");
  alignas(TOKEN_DEFAULT_DACL) BYTE defaults[256]{};
  if (!Query(raw, TokenDefaultDacl, defaults, sizeof(defaults))) return GetLastError();
  if (!SameAcl(default_acl, reinterpret_cast<const TOKEN_DEFAULT_DACL*>(defaults)->DefaultDacl)) return ERROR_ACCESS_DENIED;
  *statistics = identity.stats;
  result->reset(token.release());
  return ERROR_SUCCESS;
}

DWORD VerifyPrivateQueryAccess(const TOKEN_STATISTICS& expected) {
  HANDLE raw = nullptr;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &raw)) return GetLastError();
  const Handle query(raw);
  TOKEN_STATISTICS actual{};
  if (!Query(raw, TokenStatistics, &actual)) return GetLastError();
  if (actual.TokenId.HighPart != expected.TokenId.HighPart || actual.TokenId.LowPart != expected.TokenId.LowPart) {
    return ERROR_ACCESS_DENIED;
  }
  // Check each non-query right independently. A denial of a combined mask
  // would only establish that at least one right was unavailable.
  constexpr DWORD denied[] = {TOKEN_ASSIGN_PRIMARY, TOKEN_DUPLICATE, TOKEN_IMPERSONATE, TOKEN_QUERY_SOURCE,
      TOKEN_ADJUST_PRIVILEGES, TOKEN_ADJUST_GROUPS, TOKEN_ADJUST_DEFAULT, TOKEN_ADJUST_SESSIONID,
      DELETE, READ_CONTROL, WRITE_DAC, WRITE_OWNER};
  for (DWORD access : denied) {
    HANDLE forbidden = nullptr;
    const BOOL opened = OpenThreadToken(GetCurrentThread(), access, FALSE, &forbidden);
    const DWORD error = opened ? ERROR_SUCCESS : GetLastError();
    if (forbidden && forbidden != INVALID_HANDLE_VALUE) CloseHandle(forbidden);
    if (opened) return ERROR_ACCESS_DENIED;
    if (error != ERROR_ACCESS_DENIED) return error ? error : ERROR_GEN_FAILURE;
  }
  return ERROR_SUCCESS;
}

DWORD VerifyCapabilities(HANDLE token, const SECURITY_CAPABILITIES& expected) {
  DWORD bytes = 0;
  if (GetTokenInformation(token, TokenCapabilities, nullptr, 0, &bytes)) return ERROR_INVALID_DATA;
  const DWORD error = GetLastError();
  if (error != ERROR_INSUFFICIENT_BUFFER) return error ? error : ERROR_GEN_FAILURE;
  if (bytes < sizeof(DWORD) || bytes > 16384) return ERROR_INVALID_DATA;
  std::vector<std::byte> buffer(bytes);
  DWORD received = 0;
  if (!GetTokenInformation(token, TokenCapabilities, buffer.data(), bytes, &received)) return GetLastError();
  if (received < sizeof(DWORD) || received > bytes) return ERROR_INVALID_DATA;
  DWORD count = 0;
  std::memcpy(&count, buffer.data(), sizeof(count));
  if (count != expected.CapabilityCount) return ERROR_ACCESS_DENIED;
  if (count == 0) return ERROR_SUCCESS;
  if (count > kMaximumCapabilities ||
      offsetof(TOKEN_GROUPS, Groups) + count * sizeof(SID_AND_ATTRIBUTES) > received) return ERROR_INVALID_DATA;
  const auto* actual = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
  bool matched[kMaximumCapabilities]{};
  for (DWORD i = 0; i < count; ++i) {
    const auto& capability = actual->Groups[i];
    if (!capability.Sid || !IsValidSid(capability.Sid)) return ERROR_INVALID_SID;
    bool found = false;
    for (DWORD j = 0; j < count; ++j) {
      if (!EqualSid(capability.Sid, expected.Capabilities[j].Sid)) continue;
      if (matched[j] || capability.Attributes != expected.Capabilities[j].Attributes) return ERROR_ACCESS_DENIED;
      matched[j] = true;
      found = true;
      break;
    }
    if (!found) return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
}

DWORD SealAndVerifyToken(HANDLE token, const Identity& desktop, const AuthorityLaunchSpec& spec,
                        PACL default_acl, const char** startup_stage, bool child) {
  const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
  Identity identity;
  stage(child ? "child.token-identity" : "lowbox.token-identity");
  if (!identity.Read(token)) return ERROR_INVALID_DATA;
  const LUID& actual = identity.stats.AuthenticationId;
  const LUID& expected = desktop.stats.AuthenticationId;
  if (!identity.MatchesDesktop(spec) || identity.contained != TRUE ||
      !IsWellKnownSid(identity.Integrity(), WinLowLabelSid) || actual.LowPart != expected.LowPart ||
      actual.HighPart != expected.HighPart || !HasOnlyTraversePrivilege(token)) {
    return ERROR_ACCESS_DENIED;
  }
  alignas(TOKEN_APPCONTAINER_INFORMATION)
      BYTE buffer[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
  stage(child ? "child.appcontainer-sid" : "lowbox.appcontainer-sid");
  if (!Query(token, TokenAppContainerSid, buffer, sizeof(buffer))) return GetLastError();
  PSID package = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer;
  if (!package || !IsValidSid(package) || !EqualSid(package, spec.capabilities.AppContainerSid)) return ERROR_ACCESS_DENIED;
  stage(child ? "child.capabilities" : "lowbox.capabilities");
  const DWORD capabilities = VerifyCapabilities(token, spec.capabilities);
  if (capabilities != ERROR_SUCCESS) return capabilities;

  TOKEN_DEFAULT_DACL acl{default_acl};
  stage(child ? "child.seal-default-dacl" : "lowbox.seal-default-dacl");
  if (!SetTokenInformation(token, TokenDefaultDacl, &acl, sizeof(acl))) return GetLastError();
  alignas(TOKEN_DEFAULT_DACL) BYTE installed[256]{};
  if (!Query(token, TokenDefaultDacl, installed, sizeof(installed))) return GetLastError();
  return SameAcl(default_acl, reinterpret_cast<const TOKEN_DEFAULT_DACL*>(installed)->DefaultDacl)
      ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD SealAndVerifyChild(HANDLE process, const Identity& desktop, const AuthorityLaunchSpec& spec,
                         PACL default_acl, const char** startup_stage) {
  const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
  stage("child.open-token");
  HANDLE found = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &found)) return GetLastError();
  const Handle token(found);
  const DWORD status = SealAndVerifyToken(found, desktop, spec, default_acl, startup_stage, true);
  if (status != ERROR_SUCCESS) return status;
  DWORD session = 0;
  stage("child.desktop-session");
  if (!ProcessIdToSessionId(GetProcessId(process), &session)) return GetLastError();
  return session == spec.desktop_session ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

}  // namespace

AuthorityLaunch::~AuthorityLaunch() {
  Stop(5000);
  if (guest_.hThread) CloseHandle(guest_.hThread);
  if (guest_.hProcess) CloseHandle(guest_.hProcess);
  if (package_objects_) CloseHandle(package_objects_);
  if (package_pipe_objects_) CloseHandle(package_pipe_objects_);
}

DWORD AuthorityLaunch::Stop(DWORD timeout_milliseconds) {
  if (stop_attempted_ || !guest_.hProcess) return stop_status_;
  stop_attempted_ = true;
  DWORD wait = WaitForSingleObject(guest_.hProcess, 0);
  if (wait == WAIT_TIMEOUT) {
    if (!TerminateProcess(guest_.hProcess, ERROR_OPERATION_ABORTED)) {
      const DWORD error = GetLastError();
      if (WaitForSingleObject(guest_.hProcess, 0) != WAIT_OBJECT_0) stop_status_ = error;
    } else {
      wait = WaitForSingleObject(guest_.hProcess, timeout_milliseconds);
      if (wait != WAIT_OBJECT_0) stop_status_ = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    }
  } else if (wait != WAIT_OBJECT_0) {
    stop_status_ = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
  }
  if (stop_status_ != ERROR_SUCCESS && termination_error_) {
    DWORD expected = ERROR_SUCCESS;
    termination_error_->compare_exchange_strong(expected, stop_status_);
  }
  return stop_status_;
}

DWORD AuthorityLaunch::Create(HANDLE desktop_token, const AuthorityLaunchSpec& spec,
                              std::unique_ptr<AuthorityLaunch>* result, const char** startup_stage) try {
  const auto stage = [startup_stage](const char* value) { if (startup_stage) *startup_stage = value; };
  stage("launch.arguments");
  if (spec.namespace_create_result) *spec.namespace_create_result = {};
  if (!desktop_token || desktop_token == INVALID_HANDLE_VALUE || !result || *result ||
      !ValidSpec(spec)) return ERROR_INVALID_PARAMETER;
  stage("launch.system-caller");
  if (!LocalSystemWorker()) return ERROR_ACCESS_DENIED;

  HANDLE restricted = nullptr;
  stage("launch.restricted-token");
  if (!CreateRestrictedToken(desktop_token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr,
                             0, nullptr, &restricted)) return GetLastError();
  const Handle private_token(restricted);
  Identity desktop;
  stage("launch.parent-token-identity");
  if (!desktop.Read(restricted)) return ERROR_INVALID_DATA;
  if (!desktop.MatchesDesktop(spec) || desktop.contained ||
      !IsWellKnownSid(desktop.Integrity(), WinMediumLabelSid) ||
      !HasOnlyTraversePrivilege(restricted)) return ERROR_ACCESS_DENIED;

  LocalMemory object_descriptor, default_descriptor;
  stage("launch.security-descriptors");
  DWORD status = MakeDescriptor(kObjectPolicy, &object_descriptor);
  if (status == ERROR_SUCCESS) status = MakeDescriptor(kDefaultPolicy, &default_descriptor);
  if (status != ERROR_SUCCESS) return status;
  PACL object_acl = GetAcl(object_descriptor.get());
  PACL default_acl = GetAcl(default_descriptor.get());
  if (!object_acl || !default_acl) return ERROR_INVALID_ACL;
  TOKEN_DEFAULT_DACL default_dacl{default_acl};
  stage("launch.parent-default-dacl");
  if (!SetTokenInformation(restricted, TokenDefaultDacl, &default_dacl, sizeof(default_dacl))) {
    return GetLastError();
  }
  SECURITY_ATTRIBUTES private_objects{sizeof(private_objects), object_descriptor.get(), FALSE};
  auto launch = std::unique_ptr<AuthorityLaunch>(new (std::nothrow) AuthorityLaunch);
  if (!launch) return ERROR_NOT_ENOUGH_MEMORY;
  launch->termination_error_ = spec.termination_error;

  SIZE_T needed = 0;
  stage("launch.attributes");
  const DWORD attribute_count = spec.job ? 2 : 1;
  InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &needed);
  if (!needed) return GetLastError();
  std::vector<std::byte> memory(needed);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
  if (!InitializeProcThreadAttributeList(attributes, attribute_count, 0, &needed)) return GetLastError();
  const auto cleanup = [](LPPROC_THREAD_ATTRIBUTE_LIST list) { DeleteProcThreadAttributeList(list); };
  const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(cleanup)>
      release(attributes, cleanup);

  // The desktop is interactive. Prevent global Windows hooks and other
  // legacy extension points from loading code into the protected guest.
  DWORD64 mitigations = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
          &mitigations, sizeof(mitigations), nullptr, nullptr)) return GetLastError();
  HANDLE jobs[] = {spec.job};
  if (spec.job && !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
      jobs, sizeof(jobs), nullptr, nullptr)) return GetLastError();
  wchar_t desktop_name[] = L"winsta0\\default";
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.lpDesktop = desktop_name;
  startup.lpAttributeList = attributes;

  std::wstring command = std::wstring(L"\"") + spec.executable + L"\"";
  if (!spec.arguments.empty()) command.append(L" ").append(spec.arguments);
  if (command.size() >= 32767) return ERROR_INVALID_PARAMETER;
  std::vector<wchar_t> environment;
  stage("launch.environment-order");
  status = SortedEnvironment(spec.environment, &environment);
  if (status != ERROR_SUCCESS) return status;

  Handle session_objects_before;
  if (spec.session_objects) {
    stage("lowbox.verify-session-namespace");
    status = VerifySessionObjects(spec, SessionObjectRoot::kBase, &session_objects_before);
    if (status != ERROR_SUCCESS) return status;
  }
  Handle app_container_objects_before;
  if (spec.app_container_objects) {
    stage("lowbox.verify-appcontainer-namespace");
    status = VerifySessionObjects(spec, SessionObjectRoot::kAppContainer, &app_container_objects_before);
    if (status != ERROR_SUCCESS) return status;
  }
  PackageNamespace package_namespace;
  if (spec.app_container_objects) {
    stage("lowbox.prepare-package-namespace");
    status = package_namespace.Initialize(spec, session_objects_before.get());
    Handle package_objects, package_pipe_objects;
    if (status == ERROR_SUCCESS) {
      status = package_namespace.Prepare(spec, app_container_objects_before.get(), &package_objects, &package_pipe_objects, startup_stage);
    }
    if (status != ERROR_SUCCESS) return status;
    launch->package_objects_ = package_objects.release();
    launch->package_pipe_objects_ = package_pipe_objects.release();
  }

  // The KernelBase wrapper creates the lowbox namespace as well as its token.
  // Keep that per-user work in the verified target context, separate from the
  // SYSTEM-only process creation. There is no non-lowbox fallback.
  stage("lowbox.resolve-export");
  using CreateLowbox = BOOL(WINAPI*)(HANDLE, PSECURITY_CAPABILITIES, PHANDLE);
  const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  if (!kernelbase) {
    const DWORD error = GetLastError();
    return error ? error : ERROR_MOD_NOT_FOUND;
  }
  const auto create_lowbox = reinterpret_cast<CreateLowbox>(
      reinterpret_cast<void*>(GetProcAddress(kernelbase, "CreateAppContainerToken")));
  if (!create_lowbox) return ERROR_PROC_NOT_FOUND;
  Handle query_context;
  TOKEN_STATISTICS context_statistics{};
  status = CreatePrivateQueryContext(restricted, desktop, spec, default_acl,
      &query_context, &context_statistics, startup_stage);
  if (status != ERROR_SUCCESS) return status;
  SECURITY_CAPABILITIES capabilities = spec.capabilities;
  HANDLE raw_lowbox = nullptr;
  stage("lowbox.impersonate-target");
  if (!SetThreadToken(nullptr, query_context.get())) return GetLastError();
  stage("lowbox.verify-query-only-context");
  const DWORD context_error = VerifyPrivateQueryAccess(context_statistics);
  BOOL converted = FALSE;
  DWORD conversion_error = context_error;
  if (context_error == ERROR_SUCCESS) {
    stage("lowbox.create-token");
    // The original restricted primary remains the lowbox input. The separate
    // queryable impersonation object supplies only this thread's user context.
    converted = create_lowbox(restricted, &capabilities, &raw_lowbox);
    conversion_error = converted ? ERROR_SUCCESS : GetLastError();
  }
  if (!RevertToSelf()) {
    stage("lowbox.restore-authority-context");
    if (raw_lowbox && raw_lowbox != INVALID_HANDLE_VALUE) CloseHandle(raw_lowbox);
    std::abort();
  }
  const Handle lowbox(raw_lowbox == INVALID_HANDLE_VALUE ? nullptr : raw_lowbox);
  if (!converted) return conversion_error ? conversion_error : ERROR_GEN_FAILURE;
  if (!raw_lowbox || raw_lowbox == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
  Handle session_objects_after;
  if (spec.session_objects) {
    stage("lowbox.recheck-session-namespace");
    status = VerifySessionObjects(spec, SessionObjectRoot::kBase, &session_objects_after);
    if (status != ERROR_SUCCESS) return status;
  }
  Handle app_container_objects_after;
  if (spec.app_container_objects) {
    stage("lowbox.recheck-appcontainer-namespace");
    status = VerifySessionObjects(spec, SessionObjectRoot::kAppContainer, &app_container_objects_after);
    if (status != ERROR_SUCCESS) return status;
    stage("lowbox.reseal-package-sacl");
    status = package_namespace.SealAfterLowbox(app_container_objects_after.get(), launch->package_objects_,
        spec.namespace_create_result ? &spec.namespace_create_result->reseal : nullptr);
    if (status != ERROR_SUCCESS) return status;
    stage("lowbox.recheck-package-namespace");
    status = package_namespace.Recheck(app_container_objects_after.get(), launch->package_objects_, launch->package_pipe_objects_,
        spec.namespace_create_result ? &spec.namespace_create_result->recheck : nullptr);
    if (status != ERROR_SUCCESS) return status;
  }
  stage("lowbox.guest-default-descriptor");
  LocalMemory guest_default_descriptor;
  status = MakeGuestDefaultDescriptor(raw_lowbox, spec.capabilities.AppContainerSid, &guest_default_descriptor);
  if (status != ERROR_SUCCESS) return status;
  PACL guest_default_acl = GetAcl(guest_default_descriptor.get());
  if (!guest_default_acl) return ERROR_INVALID_ACL;
  status = SealAndVerifyToken(raw_lowbox, desktop, spec, guest_default_acl, startup_stage, false);
  if (status != ERROR_SUCCESS) return status;

  // SECURITY_CAPABILITIES is deliberately absent: the verified primary token
  // is already a lowbox. Re-converting it here would re-enter profile setup in
  // the SYSTEM context. Mitigation and atomic job attributes remain installed.
  stage("launch.create-process-as-user");
  if (!CreateProcessAsUserW(raw_lowbox, spec.executable, command.data(), &private_objects,
          &private_objects, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
          CREATE_SUSPENDED | CREATE_NO_WINDOW, environment.data(), spec.directory,
          &startup.StartupInfo, &launch->guest_)) {
    const DWORD error = GetLastError();
    return error ? error : ERROR_GEN_FAILURE;
  }

  if (spec.job) {
    stage("launch.verify-job");
    BOOL assigned = FALSE;
    if (!IsProcessInJob(launch->guest_.hProcess, spec.job, &assigned)) return GetLastError();
    if (!assigned) return ERROR_ACCESS_DENIED;
  }

  stage("launch.verify-process-acl");
  status = VerifyObject(launch->guest_.hProcess, object_acl);
  if (status == ERROR_SUCCESS) {
    stage("launch.verify-thread-acl");
    status = VerifyObject(launch->guest_.hThread, object_acl);
  }
  if (status == ERROR_SUCCESS) {
    stage("launch.verify-mitigation");
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY installed{};
    if (!GetProcessMitigationPolicy(launch->guest_.hProcess, ProcessExtensionPointDisablePolicy,
            &installed, sizeof(installed))) return GetLastError();
    if (!installed.DisableExtensionPoints) return ERROR_ACCESS_DENIED;
    status = SealAndVerifyChild(launch->guest_.hProcess, desktop, spec, guest_default_acl, startup_stage);
  }
  if (status != ERROR_SUCCESS) return status;
  if (spec.app_container_objects) {
    // Last admission check while the exact child is still suspended, before
    // publication lets the authority enroll it or hand it any resource.
    Handle base, root;
    stage("launch.recheck-session-namespace");
    status = VerifySessionObjects(spec, SessionObjectRoot::kBase, &base);
    if (status != ERROR_SUCCESS) return status;
    stage("launch.recheck-appcontainer-namespace");
    status = VerifySessionObjects(spec, SessionObjectRoot::kAppContainer, &root);
    if (status != ERROR_SUCCESS) return status;
    stage("launch.recheck-package-namespace");
    status = package_namespace.Recheck(root.get(), launch->package_objects_, launch->package_pipe_objects_,
        spec.namespace_create_result ? &spec.namespace_create_result->recheck : nullptr);
    if (status != ERROR_SUCCESS) return status;
  }
  *result = std::move(launch);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

DWORD AuthorityLaunch::Resume() {
  if (stop_attempted_) return ERROR_PROCESS_ABORTED;
  if (resumed_) return ERROR_ALREADY_ASSIGNED;
  const DWORD previous = ResumeThread(guest_.hThread);
  if (previous == static_cast<DWORD>(-1)) return GetLastError();
  if (previous != 1) {
    TerminateProcess(guest_.hProcess, ERROR_INVALID_DATA);
    return ERROR_INVALID_DATA;
  }
  resumed_ = true;
  return ERROR_SUCCESS;
}

}  // namespace gesher::usb_storage
