// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "authority_launch.h"

#include <aclapi.h>
#include <sddl.h>

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
// Windows adds the AppContainer SID when creating the lowbox token. Strip
// this token default after creation, before another thread can use it for
// default-owned objects. OWNER RIGHTS denies the desktop user's implicit DACL
// changes on newly-created objects that it may otherwise own as a user SID.
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
  status = SealAndVerifyToken(raw_lowbox, desktop, spec, default_acl, startup_stage, false);
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
    status = SealAndVerifyChild(launch->guest_.hProcess, desktop, spec, default_acl, startup_stage);
  }
  if (status != ERROR_SUCCESS) return status;
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
