// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_data_client.h"
#include "protected_data_pipe.h"

#include <aclapi.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace gesher::binder {
namespace {

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
struct FreeSecurity {
  void operator()(void* descriptor) const { LocalFree(descriptor); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;

bool ExactPackageSid(PSID sid) {
  constexpr SID_IDENTIFIER_AUTHORITY authority{SECURITY_APP_PACKAGE_AUTHORITY};
  const UCHAR count = *GetSidSubAuthorityCount(sid);
  return (count == SECURITY_PARENT_PACKAGE_RID_COUNT || count == SECURITY_CHILD_PACKAGE_RID_COUNT) &&
      std::memcmp(GetSidIdentifierAuthority(sid), &authority, sizeof(authority)) == 0 &&
      *GetSidSubAuthority(sid, 0) == SECURITY_APP_PACKAGE_BASE_RID;
}

struct ClientSids {
  alignas(TOKEN_GROUPS) BYTE groups[sizeof(TOKEN_GROUPS) + SECURITY_MAX_SID_SIZE];
  alignas(TOKEN_APPCONTAINER_INFORMATION)
      BYTE container[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE];
  PSID logon = nullptr;
  PSID package = nullptr;

  DWORD Read(DWORD session) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return GetLastError();
    const Handle token(raw);
    DWORD actual = 0, contained = 0, size = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &actual, sizeof(actual), &size) ||
        !GetTokenInformation(token.get(), TokenLogonSid, groups, sizeof(groups), &size) ||
        !GetTokenInformation(token.get(), TokenIsAppContainer, &contained, sizeof(contained), &size)) {
      return GetLastError();
    }
    const auto* credentials = reinterpret_cast<TOKEN_GROUPS*>(groups);
    constexpr DWORD required = SE_GROUP_LOGON_ID | SE_GROUP_ENABLED;
    if (actual != session || credentials->GroupCount != 1 ||
        (credentials->Groups[0].Attributes & required) != required ||
        !IsValidSid(credentials->Groups[0].Sid)) return ERROR_ACCESS_DENIED;
    logon = credentials->Groups[0].Sid;

    if (contained) {
      if (!GetTokenInformation(token.get(), TokenAppContainerSid, container, sizeof(container), &size)) {
        return GetLastError();
      }
      package = reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(container)->TokenAppContainer;
      if (!package || !IsValidSid(package) || !ExactPackageSid(package)) return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
  }
};

PSID AceSid(PACL acl, DWORD index, BYTE type, DWORD mask) {
  void* raw = nullptr;
  if (!GetAce(acl, index, &raw)) return nullptr;
  // Allow and integrity label ACEs have the same fixed prefix.
  static_assert(offsetof(ACCESS_ALLOWED_ACE, SidStart) ==
                offsetof(SYSTEM_MANDATORY_LABEL_ACE, SidStart));
  const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
  constexpr size_t sid_offset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
  if (ace->Header.AceType != type || ace->Header.AceFlags != 0 ||
      ace->Header.AceSize < sid_offset + 8 || ace->Mask != mask) return nullptr;
  PSID sid = const_cast<DWORD*>(&ace->SidStart);
  const DWORD length = GetSidLengthRequired(*GetSidSubAuthorityCount(sid));
  return sid_offset + length == ace->Header.AceSize && IsValidSid(sid) ? sid : nullptr;
}

DWORD VerifyPipeSecurity(HANDLE pipe, PSID authority, const ClientSids& expected) {
  PSID owner = nullptr;
  PACL dacl = nullptr, labels = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD error = GetSecurityInfo(pipe, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
      &owner, nullptr, &dacl, &labels, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  const std::unique_ptr<void, FreeSecurity> security(descriptor);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!owner || !IsValidSid(owner) || !EqualSid(owner, authority) || !dacl || !labels ||
      !IsValidAcl(dacl) || !IsValidAcl(labels) || dacl->AceCount < 3 || labels->AceCount != 1 ||
      !GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED)) {
    return ERROR_ACCESS_DENIED;
  }
  PSID system = AceSid(dacl, 0, ACCESS_ALLOWED_ACE_TYPE, FILE_ALL_ACCESS);
  PSID logon = AceSid(dacl, 1, ACCESS_ALLOWED_ACE_TYPE, data_pipe::kClientAccess);
  PSID label = AceSid(labels, 0, SYSTEM_MANDATORY_LABEL_ACE_TYPE,
                     SYSTEM_MANDATORY_LABEL_NO_WRITE_UP);
  if (!system || !EqualSid(system, authority) || !logon || !EqualSid(logon, expected.logon) ||
      !label || !IsWellKnownSid(label, WinLowLabelSid)) return ERROR_ACCESS_DENIED;
  bool has_package = expected.package == nullptr;
  for (DWORD i = 2; i < dacl->AceCount; ++i) {
    PSID package = AceSid(dacl, i, ACCESS_ALLOWED_ACE_TYPE, data_pipe::kClientAccess);
    if (!package || !ExactPackageSid(package)) return ERROR_ACCESS_DENIED;
    if (expected.package && EqualSid(expected.package, package)) has_package = true;
  }
  return has_package ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD VerifyServer(HANDLE pipe, const ProtectedDataEndpoint& endpoint) {
  ULONG pid = 0, session = 0;
  if (!GetNamedPipeServerProcessId(pipe, &pid) || !GetNamedPipeServerSessionId(pipe, &session)) {
    return GetLastError();
  }
  return pid == endpoint.authority_pid && session == endpoint.authority_session
      ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD Open(const ProtectedDataEndpoint& endpoint, PSID owner, HANDLE* result) try {
  if (!result || *result || !owner || !IsValidSid(owner) || endpoint.authority_pid == 0 ||
      !data_pipe::ValidLocator(endpoint.locator)) return ERROR_INVALID_PARAMETER;
  ClientSids client{};
  DWORD status = client.Read(endpoint.desktop_session);
  if (status != ERROR_SUCCESS) return status;
  HANDLE raw = CreateFileW(std::wstring(endpoint.locator).c_str(),
      data_pipe::kClientAccess, 0, nullptr, OPEN_EXISTING,
      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (raw == INVALID_HANDLE_VALUE) return GetLastError();
  Handle pipe(raw);
  DWORD flags = 0;
  if (!GetNamedPipeInfo(pipe.get(), &flags, nullptr, nullptr, nullptr)) return GetLastError();
  if (flags & (PIPE_SERVER_END | PIPE_TYPE_MESSAGE)) return ERROR_INVALID_HANDLE;
  status = VerifyServer(pipe.get(), endpoint);
  if (status == ERROR_SUCCESS) status = VerifyPipeSecurity(pipe.get(), owner, client);
  if (status == ERROR_SUCCESS) status = VerifyServer(pipe.get(), endpoint);
  if (status != ERROR_SUCCESS) return status;
  *result = pipe.release();
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

}  // namespace

DWORD OpenProtectedDataPipe(const ProtectedDataEndpoint& endpoint, HANDLE* result) {
  if (endpoint.authority_session != 0) return ERROR_INVALID_PARAMETER;
  alignas(DWORD) BYTE system[SECURITY_MAX_SID_SIZE]{};
  DWORD size = sizeof(system);
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) return GetLastError();
  return Open(endpoint, system, result);
}

#ifdef GESHER_BINDER_DATA_CLIENT_FIXTURE
DWORD OpenProtectedDataPipeForFixture(const ProtectedDataEndpoint& endpoint,
                                      PSID fixture_owner, HANDLE* result) {
  return Open(endpoint, fixture_owner, result);
}
#endif

}  // namespace gesher::binder
