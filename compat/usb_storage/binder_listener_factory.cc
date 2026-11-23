// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "binder_listener_factory.h"

#include "authority_launch.h"
#include "../binder/protected_bootstrap_wire.h"
#include "../binder/protected_data_pipe.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

namespace gesher::usb_storage {
namespace {

namespace data_pipe = binder::data_pipe;

constexpr DWORD kServerRights = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
    FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
constexpr DWORD kModes = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
constexpr DWORD kBuffer = 1 << 16;
constexpr DWORD kExitWait = 5000;
static_assert((kServerRights & FILE_CREATE_PIPE_INSTANCE) == 0);
static_assert(data_pipe::kClientAccess == 0x12018b);

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
struct FreeLocalOwner {
  void operator()(void* value) const { LocalFree(value); }
};
struct CloseServiceOwner {
  void operator()(SC_HANDLE service) const { CloseServiceHandle(service); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;
using LocalMemory = std::unique_ptr<void, FreeLocalOwner>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, CloseServiceOwner>;

DWORD VerifyOwnService() {
  HANDLE token = nullptr;
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
    CloseHandle(token);
    return ERROR_ACCESS_DENIED;
  }
  if (GetLastError() != ERROR_NO_TOKEN) return ERROR_ACCESS_DENIED;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
  const Handle release(token);
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  if (!GetTokenInformation(token, TokenUser, user, sizeof(user), &bytes)) return GetLastError();
  if (!IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid)) {
    return ERROR_ACCESS_DENIED;
  }
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) return GetLastError();
  const Service own(OpenServiceW(manager.get(), binder::bootstrap_wire::kAuthorityService,
                                  SERVICE_QUERY_STATUS));
  if (!own) return GetLastError();
  SERVICE_STATUS_PROCESS status{};
  if (!QueryServiceStatusEx(own.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
          sizeof(status), &bytes)) return GetLastError();
  return status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId == GetCurrentProcessId() &&
      status.dwServiceType == SERVICE_WIN32_OWN_PROCESS ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD NewName(std::wstring* name) {
  BYTE nonce[data_pipe::kLocatorHexDigits / 2]{};
  if (BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
    return ERROR_GEN_FAILURE;
  }
  constexpr wchar_t hex[] = L"0123456789abcdef";
  name->assign(data_pipe::kLocatorPrefix);
  for (const BYTE value : nonce) {
    name->push_back(hex[value >> 4]);
    name->push_back(hex[value & 15]);
  }
  return ERROR_SUCCESS;
}

bool ExactPackageSid(PSID sid) {
  constexpr SID_IDENTIFIER_AUTHORITY package_authority{SECURITY_APP_PACKAGE_AUTHORITY};
  return sid && IsValidSid(sid) && *GetSidSubAuthorityCount(sid) == 8 &&
      std::memcmp(GetSidIdentifierAuthority(sid), &package_authority, sizeof(package_authority)) == 0 &&
      *GetSidSubAuthority(sid, 0) == SECURITY_APP_PACKAGE_BASE_RID;
}

bool LogonSid(PSID sid) {
  constexpr SID_IDENTIFIER_AUTHORITY nt_authority{SECURITY_NT_AUTHORITY};
  return sid && IsValidSid(sid) && *GetSidSubAuthorityCount(sid) == SECURITY_LOGON_IDS_RID_COUNT &&
      std::memcmp(GetSidIdentifierAuthority(sid), &nt_authority, sizeof(nt_authority)) == 0 &&
      *GetSidSubAuthority(sid, 0) == SECURITY_LOGON_IDS_RID;
}

DWORD SidString(PSID sid, LocalMemory* text) {
  wchar_t* value = nullptr;
  if (!ConvertSidToStringSidW(sid, &value)) return GetLastError();
  text->reset(value);
  return ERROR_SUCCESS;
}

DWORD ConvertPolicy(const std::wstring& policy, LocalMemory* result) {
  PSECURITY_DESCRIPTOR value = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(policy.c_str(), SDDL_REVISION_1,
          &value, nullptr)) return GetLastError();
  result->reset(value);
  return ERROR_SUCCESS;
}

DWORD BuildPolicies(HANDLE broker, PSID owner, std::span<PSID const> packages,
                    LocalMemory* private_guard, LocalMemory* admitted) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(broker, TOKEN_QUERY, &token)) return GetLastError();
  const Handle own(token);
  alignas(TOKEN_GROUPS) BYTE buffer[sizeof(TOKEN_GROUPS) + SECURITY_MAX_SID_SIZE]{};
  DWORD bytes = 0;
  if (!GetTokenInformation(token, TokenLogonSid, buffer, sizeof(buffer), &bytes)) {
    return GetLastError();
  }
  const auto* logon = reinterpret_cast<const TOKEN_GROUPS*>(buffer);
  if (logon->GroupCount != 1 || !(logon->Groups[0].Attributes & SE_GROUP_ENABLED) ||
      (logon->Groups[0].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID ||
      !LogonSid(logon->Groups[0].Sid)) return ERROR_ACCESS_DENIED;

  LocalMemory owner_text, logon_text;
  DWORD status = SidString(owner, &owner_text);
  if (status == ERROR_SUCCESS) status = SidString(logon->Groups[0].Sid, &logon_text);
  if (status != ERROR_SUCCESS) return status;
  std::wstring privileged = L"O:";
  privileged.append(static_cast<const wchar_t*>(owner_text.get())).append(L"D:P(A;;FA;;;")
      .append(static_cast<const wchar_t*>(owner_text.get())).append(L")");
  constexpr wchar_t kLowLabel[] = L"S:(ML;;NW;;;LW)";
  status = ConvertPolicy(privileged + kLowLabel, private_guard);
  if (status != ERROR_SUCCESS) return status;
  // Both lowbox access checks must pass. Grant just the normal logon and the
  // specific enrolled packages individually. No client ACE includes the
  // FILE_APPEND_DATA bit, which grants creation of more server instances.
  std::wstring clients = privileged + L"(A;;0x12018b;;;" +
      static_cast<const wchar_t*>(logon_text.get()) + L")";
  for (PSID package : packages) {
    if (!ExactPackageSid(package)) return ERROR_INVALID_SID;
    LocalMemory text;
    status = SidString(package, &text);
    if (status != ERROR_SUCCESS) return status;
    clients.append(L"(A;;0x12018b;;;").append(static_cast<const wchar_t*>(text.get())).append(L")");
  }
  return ConvertPolicy(clients + kLowLabel, admitted);
}

DWORD PublishDacl(HANDLE guard, PSECURITY_DESCRIPTOR security) {
  PACL dacl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  if (!GetSecurityDescriptorDacl(security, &present, &dacl, &defaulted)) return GetLastError();
  if (!present || !dacl) return ERROR_INVALID_ACL;
  return SetSecurityInfo(guard, SE_KERNEL_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
}

}  // namespace

DWORD BinderListenerFactory::CreateLocator(std::wstring* locator) {
  return locator && locator->empty() ? NewName(locator) : ERROR_INVALID_PARAMETER;
}

DWORD BinderListenerFactory::Create(const AuthorityLaunch& broker, std::wstring_view locator,
    std::span<PSID const> app_containers, std::unique_ptr<BinderListenerFactory>* result) {
  const DWORD authorized = VerifyOwnService();
  if (authorized != ERROR_SUCCESS) return authorized;
  alignas(DWORD) BYTE system[SECURITY_MAX_SID_SIZE]{};
  DWORD size = sizeof(system);
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) return GetLastError();
  return CreateOwned(broker.process(), system, locator, app_containers, result);
}

#ifdef GESHER_BINDER_LISTENER_FACTORY_FIXTURE
DWORD BinderListenerFactory::CreateForFixture(HANDLE broker, PSID owner,
    std::span<PSID const> app_containers, std::unique_ptr<BinderListenerFactory>* result) {
  std::wstring locator;
  const DWORD status = CreateLocator(&locator);
  return status == ERROR_SUCCESS ? CreateOwned(broker, owner, locator, app_containers, result) : status;
}
#endif

DWORD BinderListenerFactory::CreateOwned(HANDLE broker, PSID owner, std::wstring_view locator,
    std::span<PSID const> app_containers, std::unique_ptr<BinderListenerFactory>* result) try {
  if (!broker || broker == INVALID_HANDLE_VALUE || !owner || !IsValidSid(owner) ||
      app_containers.empty() || !result || *result) return ERROR_INVALID_PARAMETER;
  constexpr std::wstring_view prefix(data_pipe::kLocatorPrefix);
  if (!locator.starts_with(prefix) || locator.size() != prefix.size() + data_pipe::kLocatorHexDigits) {
    return ERROR_INVALID_NAME;
  }
  for (const wchar_t digit : locator.substr(prefix.size())) {
    if (!((digit >= L'0' && digit <= L'9') || (digit >= L'a' && digit <= L'f'))) return ERROR_INVALID_NAME;
  }
  auto factory = std::unique_ptr<BinderListenerFactory>(new BinderListenerFactory);
  if (!DuplicateHandle(GetCurrentProcess(), broker, GetCurrentProcess(), &factory->broker_,
          PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | PROCESS_TERMINATE | SYNCHRONIZE,
          FALSE, 0)) return GetLastError();
  const DWORD pid = GetProcessId(factory->broker_);
  if (!pid) return GetLastError();
  if (pid == GetCurrentProcessId()) return ERROR_INVALID_PARAMETER;
  if (WaitForSingleObject(factory->broker_, 0) != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;

  factory->name_ = locator;
  LocalMemory private_guard, admitted;
  DWORD status = BuildPolicies(factory->broker_, owner, app_containers, &private_guard, &admitted);
  if (status != ERROR_SUCCESS) return status;
  SECURITY_ATTRIBUTES security{sizeof(security), private_guard.get(), FALSE};
  factory->guard_ = CreateNamedPipeW(factory->name_.c_str(), PIPE_ACCESS_DUPLEX | WRITE_DAC |
          FILE_FLAG_FIRST_PIPE_INSTANCE, kModes, PIPE_UNLIMITED_INSTANCES, kBuffer, kBuffer,
          0, &security);
  if (factory->guard_ == INVALID_HANDLE_VALUE) return GetLastError();
  // Occupy an owner-only first instance before exposing the client DACL. A
  // later client cannot win this connection or consume the namespace guard.
  factory->guard_client_ = CreateFileW(factory->name_.c_str(), data_pipe::kClientAccess, 0, nullptr,
      OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (factory->guard_client_ == INVALID_HANDLE_VALUE) return GetLastError();
  if (!ConnectNamedPipe(factory->guard_, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
    return GetLastError();
  }
  ULONG client = 0;
  if (!GetNamedPipeClientProcessId(factory->guard_, &client)) return GetLastError();
  if (client != GetCurrentProcessId()) return ERROR_ACCESS_DENIED;
  status = PublishDacl(factory->guard_, admitted.get());
  if (status != ERROR_SUCCESS) return status;
  if (WaitForSingleObject(factory->broker_, 0) != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;
  factory->published_ = true;
  *result = std::move(factory);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

BinderListenerFactory::~BinderListenerFactory() {
  if (published_) Revoke();
  if (guard_client_ != INVALID_HANDLE_VALUE) CloseHandle(guard_client_);
  if (guard_ != INVALID_HANDLE_VALUE) CloseHandle(guard_);
  if (broker_) CloseHandle(broker_);
}

DWORD BinderListenerFactory::Take(HANDLE* broker_listener) {
  if (!broker_listener || *broker_listener) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  if (revoked_) return ERROR_OPERATION_ABORTED;
  if (WaitForSingleObject(broker_, 0) != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;
  HANDLE raw = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX, kModes,
      PIPE_UNLIMITED_INSTANCES, kBuffer, kBuffer, 0, nullptr);
  if (raw == INVALID_HANDLE_VALUE) return GetLastError();
  const Handle listener(raw);
  HANDLE transferred = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), raw, broker_, &transferred, kServerRights, FALSE, 0)) {
    return GetLastError();
  }
  if (WaitForSingleObject(broker_, 0) != WAIT_TIMEOUT) {
    DuplicateHandle(broker_, transferred, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    return ERROR_PROCESS_ABORTED;
  }
  *broker_listener = transferred;
  return ERROR_SUCCESS;
}

DWORD BinderListenerFactory::Revoke() {
  std::lock_guard lock(mutex_);
  revoked_ = true;
  DWORD state = WaitForSingleObject(broker_, 0);
  if (state == WAIT_FAILED) return GetLastError();
  if (state == WAIT_TIMEOUT && !TerminateProcess(broker_, ERROR_OPERATION_ABORTED)) {
    const DWORD status = GetLastError();
    if (WaitForSingleObject(broker_, 0) != WAIT_OBJECT_0) return status;
  }
  state = WaitForSingleObject(broker_, kExitWait);
  if (state != WAIT_OBJECT_0) return state == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
  if (guard_client_ != INVALID_HANDLE_VALUE) CloseHandle(guard_client_);
  if (guard_ != INVALID_HANDLE_VALUE) CloseHandle(guard_);
  guard_client_ = INVALID_HANDLE_VALUE;
  guard_ = INVALID_HANDLE_VALUE;
  return ERROR_SUCCESS;
}

}  // namespace gesher::usb_storage
