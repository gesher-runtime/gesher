/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hosted_drop.h"

#include <aclapi.h>
#include <algorithm>
#include <cwchar>
#include <exception>

namespace gesher::windows {
namespace {

bool FileSystemPath(const wchar_t* path) {
  if (!path || !path[0] || !path[1] || !path[2]) return false;
  const auto drive_letter = [](wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
  };
  const bool drive = drive_letter(path[0]) && path[1] == L':' && path[2] == L'\\';
  const bool unc = path[0] == L'\\' && path[1] == L'\\' &&
      path[2] != L'.' && path[2] != L'?' && path[2] != L'\\';
  if (!drive && !unc) return false;
  // Skip the drive separator. Reject device paths and alternate data streams.
  return wcschr(path + (drive ? 2 : 0), L':') == nullptr &&
      wcschr(path, L'/') == nullptr;
}

bool IsOwnedWindow(HWND hwnd, HANDLE process, DWORD* window_thread) {
  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
  if (tid == 0 || pid == 0 || GetProcessId(process) != pid ||
      WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return false;
  *window_thread = tid;
  return true;
}


constexpr DWORD kMaximumTokenInfo = 64 * 1024;
bool QueryToken(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<BYTE>* bytes) {
  DWORD size = 0;
  if (GetTokenInformation(token, kind, nullptr, 0, &size) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > kMaximumTokenInfo) return false;
  bytes->resize(size);
  return GetTokenInformation(token, kind, bytes->data(), size, &size) != FALSE;
}

bool EquivalentGroups(HANDLE source, HANDLE authority) {
  std::vector<BYTE> left, right;
  if (!QueryToken(source, TokenGroups, &left) || !QueryToken(authority, TokenGroups, &right)) return false;
  const auto& a = *reinterpret_cast<const TOKEN_GROUPS*>(left.data());
  const auto& b = *reinterpret_cast<const TOKEN_GROUPS*>(right.data());
  if (a.GroupCount != b.GroupCount) return false;
  for (DWORD i = 0; i < a.GroupCount; ++i) {
    const auto& member = a.Groups[i];
    const auto found = std::find_if(b.Groups, b.Groups + b.GroupCount, [&](const SID_AND_ATTRIBUTES& other) {
      return member.Attributes == other.Attributes && EqualSid(member.Sid, other.Sid);
    });
    if (found == b.Groups + b.GroupCount) return false;
  }
  return true;
}

bool EquivalentPrivileges(HANDLE source, HANDLE authority) {
  std::vector<BYTE> left, right;
  if (!QueryToken(source, TokenPrivileges, &left) || !QueryToken(authority, TokenPrivileges, &right)) return false;
  const auto& a = *reinterpret_cast<const TOKEN_PRIVILEGES*>(left.data());
  const auto& b = *reinterpret_cast<const TOKEN_PRIVILEGES*>(right.data());
  if (a.PrivilegeCount != b.PrivilegeCount) return false;
  for (DWORD i = 0; i < a.PrivilegeCount; ++i) {
    const auto& privilege = a.Privileges[i];
    const auto found = std::find_if(b.Privileges, b.Privileges + b.PrivilegeCount,
        [&](const LUID_AND_ATTRIBUTES& other) {
      return privilege.Luid.LowPart == other.Luid.LowPart && privilege.Luid.HighPart == other.Luid.HighPart &&
          privilege.Attributes == other.Attributes;
    });
    if (found == b.Privileges + b.PrivilegeCount) return false;
  }
  return true;
}

bool EquivalentAccess(HANDLE source, HANDLE authority) {
  std::vector<BYTE> left, right;
  for (auto kind : {TokenElevationType, TokenMandatoryPolicy, TokenUIAccess, TokenVirtualizationEnabled,
                    TokenHasRestrictions, TokenIsRestricted}) {
    if (!QueryToken(source, kind, &left) || !QueryToken(authority, kind, &right) || left != right) return false;
  }
  return EquivalentGroups(source, authority) && EquivalentPrivileges(source, authority);
}

// This flag is set only after this dedicated process installed the COM access
// descriptor, before the first interface export. Never enable it in ART.
bool g_secured_drop_worker = false;

void RetireOnFailedRevert() {
  // A broker cannot process new RPC operations while still impersonating.
  TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
}

}  // namespace


HRESULT ConfigureDropWorkerCom() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return HRESULT_FROM_WIN32(GetLastError());
  process_identity::Principal desktop;
  const bool medium = process_identity::ReadPrincipal(token, &desktop) && !desktop.restricted &&
      desktop.container.size == 0 && desktop.integrity == SECURITY_MANDATORY_MEDIUM_RID;
  CloseHandle(token);
  if (!medium) return E_ACCESSDENIED;

  // S-1-15-2-1 and S-1-15-2-2 (normal/restricted application packages).
  const SID_IDENTIFIER_AUTHORITY package_authority{SECURITY_APP_PACKAGE_AUTHORITY};
  alignas(DWORD) BYTE all[SECURITY_MAX_SID_SIZE]{}, restricted[SECURITY_MAX_SID_SIZE]{};
  if (!InitializeSid(all, const_cast<SID_IDENTIFIER_AUTHORITY*>(&package_authority), 2) ||
      !InitializeSid(restricted, const_cast<SID_IDENTIFIER_AUTHORITY*>(&package_authority), 2)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  *GetSidSubAuthority(all, 0) = *GetSidSubAuthority(restricted, 0) = 2;
  *GetSidSubAuthority(all, 1) = 1;
  *GetSidSubAuthority(restricted, 1) = 2;
  PSID owner = desktop.account.bytes.data();
  constexpr DWORD flags = COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL;
  EXPLICIT_ACCESSW entries[3]{};
  for (size_t index = 0; index < std::size(entries); ++index) {
    auto& entry = entries[index];
    entry.grfAccessPermissions = flags;
    entry.grfAccessMode = index == 2 ? GRANT_ACCESS : DENY_ACCESS;
    BuildTrusteeWithSidW(&entry.Trustee, index == 0 ? all : index == 1 ? restricted : owner);
  }
  PACL acl = nullptr;
  const DWORD acl_error = SetEntriesInAclW(std::size(entries), entries, nullptr, &acl);
  if (acl_error) return HRESULT_FROM_WIN32(acl_error);
  SECURITY_DESCRIPTOR descriptor{};
  HRESULT result = HRESULT_FROM_WIN32(ERROR_INVALID_SECURITY_DESCR);
  if (InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
      SetSecurityDescriptorOwner(&descriptor, owner, FALSE) &&
      SetSecurityDescriptorGroup(&descriptor, owner, FALSE) &&
      SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) {
    result = CoInitializeSecurity(&descriptor, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                 RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr);
  }
  LocalFree(acl);
  g_secured_drop_worker = SUCCEEDED(result);
  return result;
}

OleDropSource::~OleDropSource() {
  if (token_) CloseHandle(token_);
}

bool OleDropSource::Capture() {
  if (!g_secured_drop_worker || token_) return false;
  DWORD authn = 0;
  if (FAILED(CoQueryClientBlanket(nullptr, nullptr, nullptr, &authn, nullptr, nullptr, nullptr)) ||
      authn < RPC_C_AUTHN_LEVEL_PKT_PRIVACY || FAILED(CoImpersonateClient())) return false;
  HANDLE source = nullptr;
  OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &source);
  if (FAILED(CoRevertToSelf())) {
    RetireOnFailedRevert();
    if (source) CloseHandle(source);
    return false;
  }
  if (!source) return false;
  HANDLE authority = nullptr;
  SECURITY_IMPERSONATION_LEVEL level{};
  DWORD length = 0;
  process_identity::Principal caller, worker;
  const bool authorized =
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &authority) &&
      GetTokenInformation(source, TokenImpersonationLevel, &level, sizeof(level), &length) &&
      level >= SecurityIdentification && process_identity::ReadPrincipal(source, &caller) &&
      process_identity::ReadPrincipal(authority, &worker) && caller == worker &&
      (level >= SecurityImpersonation || EquivalentAccess(source, authority)) &&
      DuplicateTokenEx(level >= SecurityImpersonation ? source : authority,
          TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token_);
  if (authority) CloseHandle(authority);
  CloseHandle(source);
  return authorized;
}

HANDLE OleDropSource::Open(const wchar_t* path) const {
  if (!token_ || !FileSystemPath(path) || !ImpersonateLoggedOnUser(token_)) {
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }
  HANDLE result = CreateFileW(path, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  DWORD error = result == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  if (!RevertToSelf()) {
    RetireOnFailedRevert();
    error = ERROR_ACCESS_DENIED;
  }
  if (result != INVALID_HANDLE_VALUE && GetFileType(result) != FILE_TYPE_DISK) {
    error = ERROR_INVALID_DATA;
  }
  if (error != ERROR_SUCCESS && result != INVALID_HANDLE_VALUE) {
    CloseHandle(result);
    result = INVALID_HANDLE_VALUE;
  }
  SetLastError(error);
  return result;
}

HostedDropWindow::~HostedDropWindow() {
  Revoke();
}

HRESULT HostedDropWindow::Register(HWND hwnd, HANDLE guest_process, IDropTarget* target) {
  if (process_ || !hwnd || !guest_process || !target) return E_INVALIDARG;
  process_identity::Process guest;
  DWORD window_thread = 0;
  if (!process_identity::ReadLiveProcess(guest_process, &guest) ||
      guest.principal.container.size == 0 ||
      !IsOwnedWindow(hwnd, guest_process, &window_thread)) return E_ACCESSDENIED;
  HANDLE retained = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), guest_process, GetCurrentProcess(), &retained,
                       PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  process_ = retained;
  hwnd_ = hwnd;
  thread_id_ = window_thread;
  owner_thread_ = GetCurrentThreadId();
  const HRESULT result = Live() ? RegisterDragDrop(hwnd_, target) : E_ACCESSDENIED;
  if (FAILED(result)) {
    CloseHandle(process_);
    process_ = nullptr;
    hwnd_ = nullptr;
    return result;
  }
  if (!Live()) {
    Revoke();
    return E_ACCESSDENIED;
  }
  return S_OK;
}

bool HostedDropWindow::Live() const {
  DWORD current_thread = 0;
  return process_ && IsOwnedWindow(hwnd_, process_, &current_thread) && current_thread == thread_id_;
}

void HostedDropWindow::Revoke() {
  if (!process_) return;
  if (GetCurrentThreadId() != owner_thread_) std::terminate();
  // A numeric HWND can point to an unrelated window after its owner exits.
  if (Live()) RevokeDragDrop(hwnd_);
  CloseHandle(process_);
  process_ = nullptr;
  hwnd_ = nullptr;
}

}  // namespace gesher::windows
