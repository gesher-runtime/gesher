// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "authority_launch.h"

#include <aclapi.h>
#include <sddl.h>

#include <cstddef>
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

template <class T>
bool Query(HANDLE token, TOKEN_INFORMATION_CLASS kind, T* result, DWORD size = sizeof(T)) {
  DWORD received = 0;
  return GetTokenInformation(token, kind, result, size, &received) != FALSE;
}

struct Identity {
  TOKEN_STATISTICS stats{};
  DWORD session = 0;
  DWORD contained = 0;
  DWORD elevated = 0;
  DWORD ui_access = 0;
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  alignas(TOKEN_MANDATORY_LABEL)
      BYTE integrity[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};

  bool Read(HANDLE token) {
    return Query(token, TokenStatistics, &stats) && Query(token, TokenSessionId, &session) &&
        Query(token, TokenIsAppContainer, &contained) && Query(token, TokenElevation, &elevated) &&
        Query(token, TokenUIAccess, &ui_access) && Query(token, TokenUser, user, sizeof(user)) &&
        Query(token, TokenIntegrityLevel, integrity, sizeof(integrity)) &&
        IsValidSid(User()) && IsValidSid(Integrity());
  }
  PSID User() const { return reinterpret_cast<const TOKEN_USER*>(user)->User.Sid; }
  PSID Integrity() const {
    return reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity)->Label.Sid;
  }
  bool MatchesDesktop(const AuthorityLaunchSpec& spec) const {
    return stats.TokenType == TokenPrimary && session == spec.desktop_session &&
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

bool ValidSpec(const AuthorityLaunchSpec& spec) {
  return spec.executable && spec.directory && AbsolutePath(spec.executable) &&
      AbsolutePath(spec.directory) && spec.arguments.size() <= 32767 &&
      spec.arguments.find(L'\0') == std::wstring_view::npos &&
      CompleteEnvironment(spec.environment) && spec.desktop_user &&
      IsValidSid(spec.desktop_user) && spec.desktop_session != 0 &&
      spec.capabilities.AppContainerSid && IsValidSid(spec.capabilities.AppContainerSid) &&
      (!spec.capabilities.CapabilityCount || spec.capabilities.Capabilities) &&
      spec.capabilities.Reserved == 0;
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

DWORD SealAndVerifyChild(HANDLE process, const Identity& desktop, const AuthorityLaunchSpec& spec,
                         PACL default_acl) {
  HANDLE found = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &found)) return GetLastError();
  const Handle token(found);
  Identity child;
  if (!child.Read(found)) return ERROR_INVALID_DATA;
  const LUID& actual = child.stats.AuthenticationId;
  const LUID& expected = desktop.stats.AuthenticationId;
  if (!child.MatchesDesktop(spec) || child.contained != TRUE ||
      !IsWellKnownSid(child.Integrity(), WinLowLabelSid) || actual.LowPart != expected.LowPart ||
      actual.HighPart != expected.HighPart || !HasOnlyTraversePrivilege(found)) {
    return ERROR_ACCESS_DENIED;
  }
  alignas(TOKEN_APPCONTAINER_INFORMATION)
      BYTE buffer[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
  if (!Query(found, TokenAppContainerSid, buffer, sizeof(buffer))) return GetLastError();
  PSID package = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(buffer)->TokenAppContainer;
  if (!package || !EqualSid(package, spec.capabilities.AppContainerSid)) return ERROR_ACCESS_DENIED;
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetProcessId(process), &session)) return GetLastError();
  if (session != spec.desktop_session) return ERROR_ACCESS_DENIED;

  TOKEN_DEFAULT_DACL acl{default_acl};
  if (!SetTokenInformation(found, TokenDefaultDacl, &acl, sizeof(acl))) return GetLastError();
  alignas(TOKEN_DEFAULT_DACL) BYTE installed[256]{};
  if (!Query(found, TokenDefaultDacl, installed, sizeof(installed))) return GetLastError();
  return SameAcl(default_acl, reinterpret_cast<const TOKEN_DEFAULT_DACL*>(installed)->DefaultDacl)
      ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

}  // namespace

AuthorityLaunch::~AuthorityLaunch() {
  if (guest_.hProcess && WaitForSingleObject(guest_.hProcess, 0) == WAIT_TIMEOUT) {
    TerminateProcess(guest_.hProcess, ERROR_OPERATION_ABORTED);
  }
  if (guest_.hThread) CloseHandle(guest_.hThread);
  if (guest_.hProcess) CloseHandle(guest_.hProcess);
}

DWORD AuthorityLaunch::Create(HANDLE desktop_token, const AuthorityLaunchSpec& spec,
                              std::unique_ptr<AuthorityLaunch>* result) try {
  if (!desktop_token || desktop_token == INVALID_HANDLE_VALUE || !result || *result ||
      !ValidSpec(spec)) return ERROR_INVALID_PARAMETER;
  if (!LocalSystemWorker()) return ERROR_ACCESS_DENIED;

  HANDLE restricted = nullptr;
  if (!CreateRestrictedToken(desktop_token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr,
                             0, nullptr, &restricted)) return GetLastError();
  const Handle private_token(restricted);
  Identity desktop;
  if (!desktop.Read(restricted)) return ERROR_INVALID_DATA;
  if (!desktop.MatchesDesktop(spec) || desktop.contained ||
      !IsWellKnownSid(desktop.Integrity(), WinMediumLabelSid) ||
      !HasOnlyTraversePrivilege(restricted)) return ERROR_ACCESS_DENIED;

  LocalMemory object_descriptor, default_descriptor;
  DWORD status = MakeDescriptor(kObjectPolicy, &object_descriptor);
  if (status == ERROR_SUCCESS) status = MakeDescriptor(kDefaultPolicy, &default_descriptor);
  if (status != ERROR_SUCCESS) return status;
  PACL object_acl = GetAcl(object_descriptor.get());
  PACL default_acl = GetAcl(default_descriptor.get());
  if (!object_acl || !default_acl) return ERROR_INVALID_ACL;
  TOKEN_DEFAULT_DACL default_dacl{default_acl};
  if (!SetTokenInformation(restricted, TokenDefaultDacl, &default_dacl, sizeof(default_dacl))) {
    return GetLastError();
  }
  SECURITY_ATTRIBUTES private_objects{sizeof(private_objects), object_descriptor.get(), FALSE};
  auto launch = std::unique_ptr<AuthorityLaunch>(new (std::nothrow) AuthorityLaunch);
  if (!launch) return ERROR_NOT_ENOUGH_MEMORY;

  SIZE_T needed = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &needed);
  if (!needed) return GetLastError();
  std::vector<std::byte> memory(needed);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
  if (!InitializeProcThreadAttributeList(attributes, 2, 0, &needed)) return GetLastError();
  const auto cleanup = [](LPPROC_THREAD_ATTRIBUTE_LIST list) { DeleteProcThreadAttributeList(list); };
  const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(cleanup)>
      release(attributes, cleanup);

  SECURITY_CAPABILITIES capabilities = spec.capabilities;
  // The desktop is interactive. Prevent global Windows hooks and other
  // legacy extension points from loading code into the protected guest.
  DWORD64 mitigations = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON;
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
          &capabilities, sizeof(capabilities), nullptr, nullptr) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
          &mitigations, sizeof(mitigations), nullptr, nullptr)) return GetLastError();
  wchar_t desktop_name[] = L"winsta0\\default";
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.lpDesktop = desktop_name;
  startup.lpAttributeList = attributes;

  std::wstring command = std::wstring(L"\"") + spec.executable + L"\"";
  if (!spec.arguments.empty()) command.append(L" ").append(spec.arguments);
  if (command.size() >= 32767) return ERROR_INVALID_PARAMETER;
  std::vector<wchar_t> environment(spec.environment.begin(), spec.environment.end());
  if (!CreateProcessAsUserW(restricted, spec.executable, command.data(), &private_objects,
          &private_objects, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
          CREATE_SUSPENDED | CREATE_NO_WINDOW, environment.data(), spec.directory,
          &startup.StartupInfo, &launch->guest_)) return GetLastError();

  status = VerifyObject(launch->guest_.hProcess, object_acl);
  if (status == ERROR_SUCCESS) status = VerifyObject(launch->guest_.hThread, object_acl);
  if (status == ERROR_SUCCESS) {
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY installed{};
    if (!GetProcessMitigationPolicy(launch->guest_.hProcess, ProcessExtensionPointDisablePolicy,
            &installed, sizeof(installed))) return GetLastError();
    if (!installed.DisableExtensionPoints) return ERROR_ACCESS_DENIED;
    status = SealAndVerifyChild(launch->guest_.hProcess, desktop, spec, default_acl);
  }
  if (status != ERROR_SUCCESS) return status;
  *result = std::move(launch);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

DWORD AuthorityLaunch::Resume() {
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
