// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>

namespace gesher::process_identity {

struct Sid {
  alignas(DWORD) std::array<BYTE, SECURITY_MAX_SID_SIZE> bytes{};
  DWORD size = 0;

  bool CopyFrom(PSID sid) {
    if (sid == nullptr || !IsValidSid(sid)) return false;
    const DWORD length = GetLengthSid(sid);
    if (length > bytes.size() || !CopySid(length, bytes.data(), sid)) return false;
    size = length;
    return true;
  }

  bool Matches(PSID sid) const {
    return size != 0 && sid != nullptr && IsValidSid(sid) &&
           EqualSid(const_cast<BYTE*>(bytes.data()), sid);
  }

  bool operator==(const Sid& other) const {
    return size == other.size && (size == 0 || Matches(const_cast<BYTE*>(other.bytes.data())));
  }
};

struct Principal {
  Sid account;
  Sid container;
  LUID logon{};
  DWORD session = 0;
  DWORD integrity = 0;
  bool restricted = false;

  bool operator==(const Principal& other) const {
    return account == other.account && container == other.container &&
           logon.LowPart == other.logon.LowPart && logon.HighPart == other.logon.HighPart &&
           session == other.session && integrity == other.integrity && restricted == other.restricted;
  }
};

inline bool ReadPrincipal(HANDLE token, Principal* result) {
  Principal principal;
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  alignas(TOKEN_MANDATORY_LABEL) BYTE label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
  DWORD size = 0, contained = 0;
  TOKEN_STATISTICS statistics{};
  if (!GetTokenInformation(token, TokenUser, user, sizeof(user), &size) ||
      !principal.account.CopyFrom(reinterpret_cast<const TOKEN_USER*>(user)->User.Sid) ||
      !GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &size) ||
      !GetTokenInformation(token, TokenSessionId, &principal.session, sizeof(principal.session), &size) ||
      !GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &size) ||
      !GetTokenInformation(token, TokenIsAppContainer, &contained, sizeof(contained), &size)) {
    return false;
  }
  principal.logon = statistics.AuthenticationId;
  principal.restricted = IsTokenRestricted(token) != FALSE;
  const PSID integrity = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
  if (integrity == nullptr || !IsValidSid(integrity)) return false;
  const BYTE count = *GetSidSubAuthorityCount(integrity);
  if (count == 0) return false;
  principal.integrity = *GetSidSubAuthority(integrity, count - 1);
  if (contained != 0) {
    alignas(TOKEN_APPCONTAINER_INFORMATION)
        BYTE container[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
    if (!GetTokenInformation(token, TokenAppContainerSid, container, sizeof(container), &size) ||
        !principal.container.CopyFrom(
            reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(container)->TokenAppContainer)) {
      return false;
    }
  }
  *result = principal;
  return true;
}

struct Process {
  DWORD pid = 0;
  uint64_t birth = 0;
  Principal principal;
};

// Inspect the retained handle returned by CreateProcess, with query and
// synchronization rights. A transport PID must never open an enrollment.
inline bool ReadLiveProcess(HANDLE process, Process* result) {
  Process snapshot;
  FILETIME birth{}, exit{}, kernel{}, user{};
  if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT ||
      (snapshot.pid = GetProcessId(process)) == 0 ||
      !GetProcessTimes(process, &birth, &exit, &kernel, &user)) {
    return false;
  }
  snapshot.birth = (static_cast<uint64_t>(birth.dwHighDateTime) << 32) | birth.dwLowDateTime;
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
  const bool captured = ReadPrincipal(token, &snapshot.principal);
  CloseHandle(token);
  if (!captured || snapshot.birth == 0 || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return false;
  *result = snapshot;
  return true;
}

// Call after reading the request on this server-side pipe.
// Terminate if reverting impersonation fails, as required by the Win32 API.
inline bool ReadPipePrincipal(HANDLE pipe, Principal* result) {
  if (!ImpersonateNamedPipeClient(pipe)) return false;
  HANDLE token = nullptr;
  const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != FALSE;
  if (!RevertToSelf()) {
    TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
    return false;
  }
  if (!opened) return false;
  const bool queried = ReadPrincipal(token, result);
  CloseHandle(token);
  return queried;
}

// Windows supplies both the connecting PID and the sending token. The
// enrollment must already own the exact live process object.
inline bool MatchPipeSender(HANDLE pipe, HANDLE enrolled_handle, const Process& enrolled) {
  ULONG pid = 0;
  Principal sender;
  return GetNamedPipeClientProcessId(pipe, &pid) && pid == enrolled.pid &&
         GetProcessId(enrolled_handle) == enrolled.pid &&
         WaitForSingleObject(enrolled_handle, 0) == WAIT_TIMEOUT &&
         ReadPipePrincipal(pipe, &sender) && sender == enrolled.principal &&
         WaitForSingleObject(enrolled_handle, 0) == WAIT_TIMEOUT;
}

}  // namespace gesher::process_identity
