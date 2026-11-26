// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "authority_peer.h"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string_view>

namespace gesher::usb_storage {
namespace {

template <typename T>
DWORD QueryToken(HANDLE token, TOKEN_INFORMATION_CLASS kind, T* value,
                 DWORD size = sizeof(T)) {
  DWORD received = 0;
  return GetTokenInformation(token, kind, value, size, &received)
      ? ERROR_SUCCESS : GetLastError();
}

struct TokenIdentity {
  TOKEN_STATISTICS statistics{};
  DWORD session = 0;
  alignas(TOKEN_USER) std::byte user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  alignas(TOKEN_APPCONTAINER_INFORMATION)
      std::byte container[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE];

  PSID UserSid() const { return reinterpret_cast<const TOKEN_USER*>(user)->User.Sid; }
  PSID ContainerSid() const {
    return reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(container)->TokenAppContainer;
  }

  DWORD Read(HANDLE token) {
    DWORD contained = 0;
    DWORD status = QueryToken(token, TokenIsAppContainer, &contained);
    if (status != ERROR_SUCCESS) return status;
    if (contained != TRUE) return ERROR_ACCESS_DENIED;
    status = QueryToken(token, TokenUser, user, sizeof(user));
    if (status != ERROR_SUCCESS) return status;
    status = QueryToken(token, TokenAppContainerSid, container, sizeof(container));
    if (status != ERROR_SUCCESS) return status;
    if (!IsValidSid(UserSid()) || !IsValidSid(ContainerSid())) return ERROR_INVALID_SID;
    status = QueryToken(token, TokenSessionId, &session);
    if (status != ERROR_SUCCESS) return status;
    return QueryToken(token, TokenStatistics, &statistics);
  }
};

DWORD RequireLiveProcess(HANDLE process) {
  const DWORD wait = WaitForSingleObject(process, 0);
  if (wait == WAIT_TIMEOUT) return ERROR_SUCCESS;
  return wait == WAIT_FAILED ? GetLastError() : ERROR_PROCESS_ABORTED;
}

// In the error case Microsoft requires ceasing execution because the thread
// may still be impersonating. Never return work to a privileged worker pool.
bool CaptureClientToken(HANDLE pipe, HANDLE* token) {
  if (!ImpersonateNamedPipeClient(pipe)) return false;
  const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, token);
  const DWORD error = opened ? ERROR_SUCCESS : GetLastError();
  if (!RevertToSelf()) std::abort();
  if (!opened) SetLastError(error);
  return opened != FALSE;
}

}  // namespace

AuthorityPeer::~AuthorityPeer() {
  if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
  if (token_) CloseHandle(token_);
  if (process_) CloseHandle(process_);
}

DWORD AuthorityPeer::Create(HANDLE launched_process, const wchar_t* local_pipe_name,
                            SECURITY_ATTRIBUTES* pipe_security,
                            std::unique_ptr<AuthorityPeer>* result) {
  if (!launched_process || launched_process == INVALID_HANDLE_VALUE ||
      !local_pipe_name || !pipe_security || pipe_security->nLength != sizeof(*pipe_security) ||
      !pipe_security->lpSecurityDescriptor ||
      pipe_security->bInheritHandle || !result || *result) {
    return ERROR_INVALID_PARAMETER;
  }
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  if (!GetSecurityDescriptorDacl(pipe_security->lpSecurityDescriptor,
          &dacl_present, &dacl, &dacl_defaulted)) return GetLastError();
  if (!dacl_present || !dacl) return ERROR_INVALID_ACL;
  // This desktop authority serves a Win32 AppContainer client through the
  // host pipe namespace. Disallow remote names and namespace components.
  constexpr std::wstring_view prefix = LR"(\\.\pipe\)";
  const std::wstring_view name(local_pipe_name);
  if (!name.starts_with(prefix) || name.size() == prefix.size() ||
      name.find_first_of(L"\\/", prefix.size()) != std::wstring_view::npos) {
    return ERROR_INVALID_NAME;
  }
  std::unique_ptr<AuthorityPeer> peer(new (std::nothrow) AuthorityPeer);
  if (!peer) return ERROR_NOT_ENOUGH_MEMORY;
  const HANDLE self = GetCurrentProcess();
  if (!DuplicateHandle(self, launched_process, self, &peer->process_,
                       PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
    return GetLastError();
  }
  DWORD status = RequireLiveProcess(peer->process_);
  if (status != ERROR_SUCCESS) return status;
  peer->pid_ = GetProcessId(peer->process_);
  if (!peer->pid_ || !ProcessIdToSessionId(peer->pid_, &peer->session_) ||
      !OpenProcessToken(peer->process_, TOKEN_QUERY, &peer->token_)) {
    return GetLastError();
  }
  TokenIdentity expected;
  status = expected.Read(peer->token_);
  if (status != ERROR_SUCCESS) return status;
  if (expected.statistics.TokenType != TokenPrimary || expected.session != peer->session_) {
    return ERROR_ACCESS_DENIED;
  }
  peer->pipe_ = CreateNamedPipeW(local_pipe_name,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, pipe_security);
  if (peer->pipe_ == INVALID_HANDLE_VALUE) return GetLastError();
  status = RequireLiveProcess(peer->process_);
  if (status != ERROR_SUCCESS) return status;
  *result = std::move(peer);
  return ERROR_SUCCESS;
}

DWORD AuthorityPeer::CheckPipeIdentity() const {
  DWORD status = RequireLiveProcess(process_);
  if (status != ERROR_SUCCESS) return status;
  ULONG pid = 0, session = 0;
  if (!GetNamedPipeClientProcessId(pipe_, &pid) || !GetNamedPipeClientSessionId(pipe_, &session)) {
    return GetLastError();
  }
  return pid == pid_ && session == session_ ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD AuthorityPeer::Authenticate() const {
  DWORD status = CheckPipeIdentity();
  if (status != ERROR_SUCCESS) return status;
  HANDLE token = nullptr;
  // Don't silently remove an unrelated impersonation on return.
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
    CloseHandle(token);
    return ERROR_BAD_IMPERSONATION_LEVEL;
  }
  if (GetLastError() != ERROR_NO_TOKEN) return ERROR_BAD_IMPERSONATION_LEVEL;
  if (!CaptureClientToken(pipe_, &token)) return GetLastError();
  TokenIdentity actual;
  status = actual.Read(token);
  CloseHandle(token);
  if (status != ERROR_SUCCESS) return status;
  TokenIdentity expected;
  status = expected.Read(token_);
  if (status != ERROR_SUCCESS) return status;
  const auto& actual_stats = actual.statistics;
  const auto& actual_auth = actual_stats.AuthenticationId;
  const auto& expected_auth = expected.statistics.AuthenticationId;
  if (actual_stats.TokenType != TokenImpersonation ||
      actual_stats.ImpersonationLevel < SecurityIdentification ||
      expected.session != session_ || actual.session != expected.session ||
      actual_auth.LowPart != expected_auth.LowPart || actual_auth.HighPart != expected_auth.HighPart ||
      !EqualSid(actual.UserSid(), expected.UserSid()) ||
      !EqualSid(actual.ContainerSid(), expected.ContainerSid())) {
    return ERROR_ACCESS_DENIED;
  }
  return CheckPipeIdentity();
}

}  // namespace gesher::usb_storage
