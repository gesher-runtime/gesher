// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_bootstrap.h"

#include "protected_bootstrap_wire.h"

#include <aclapi.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace gesher::binder {
namespace {

namespace wire = bootstrap_wire;
constexpr DWORD kHandshakeTimeout = 5000;
constexpr DWORD kReplyTimeout = 5000;
constexpr DWORD kAuthorityPollInterval = 1000;

struct CloseHandleOwner {
  void operator()(void* value) const { CloseHandle(value); }
};
struct CloseServiceOwner {
  void operator()(SC_HANDLE value) const { CloseServiceHandle(value); }
};
struct FreeLocalOwner {
  void operator()(void* value) const { LocalFree(value); }
};
struct FreeTaskOwner {
  void operator()(void* value) const { CoTaskMemFree(value); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;
using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, CloseServiceOwner>;

bool ValidLocator(std::wstring_view path) {
  constexpr std::wstring_view prefix(wire::kPipePrefix);
  if (!path.starts_with(prefix) || path.size() != prefix.size() + wire::kLocatorHexDigits) {
    return false;
  }
  return std::all_of(path.begin() + prefix.size(), path.end(), [](wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
  });
}

bool SameText(const wchar_t* field, const wchar_t* expected) {
  return field && CompareStringOrdinal(field, -1, expected, -1, TRUE) == CSTR_EQUAL;
}

DWORD CheckServiceConfig(SC_HANDLE service) {
  DWORD size = 0;
  if (QueryServiceConfigW(service, nullptr, 0, &size)) return ERROR_INVALID_DATA;
  const DWORD error = GetLastError();
  if (error != ERROR_INSUFFICIENT_BUFFER || size < sizeof(QUERY_SERVICE_CONFIGW) ||
      size > 8192) return error == ERROR_INSUFFICIENT_BUFFER ? ERROR_INVALID_DATA : error;
  std::vector<std::byte> bytes(size);
  auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
  if (!QueryServiceConfigW(service, config, size, &size)) return GetLastError();
  PWSTR program_files = nullptr;
  const HRESULT path = SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DONT_VERIFY,
                                            nullptr, &program_files);
  const std::unique_ptr<wchar_t, FreeTaskOwner> folder(program_files);
  if (FAILED(path) || !folder) return ERROR_PATH_NOT_FOUND;
  const std::wstring image = L"\"" + std::wstring(folder.get()) +
      LR"(\Gesher Raw USB Broker\gesher-usb-authority.exe" service)";
  if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
      config->dwStartType != SERVICE_DEMAND_START ||
      !SameText(config->lpServiceStartName, L"LocalSystem") ||
      !SameText(config->lpBinaryPathName, image.c_str())) {
    return ERROR_ACCESS_DENIED;
  }
  return ERROR_SUCCESS;
}

DWORD CheckPipeSecurity(HANDLE pipe) {
  PSID owner = nullptr;
  PACL dacl = nullptr, label = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  DWORD error = GetSecurityInfo(pipe, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
      &owner, nullptr, &dacl, &label, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  const std::unique_ptr<void, FreeLocalOwner> security(descriptor);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!owner || !IsWellKnownSid(owner, WinLocalSystemSid) || !dacl ||
      !IsValidAcl(dacl) || dacl->AceCount != 3 || !label || !IsValidAcl(label) ||
      label->AceCount != 1 || !GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      !(control & SE_DACL_PROTECTED)) return ERROR_ACCESS_DENIED;

  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return GetLastError();
  const Handle token(raw_token);
  alignas(TOKEN_USER) std::byte user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  alignas(TOKEN_APPCONTAINER_INFORMATION)
      std::byte container[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE];
  DWORD bytes = 0;
  if (!GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &bytes) ||
      !GetTokenInformation(token.get(), TokenAppContainerSid, container, sizeof(container), &bytes)) {
    return GetLastError();
  }
  PSID user_sid = reinterpret_cast<const TOKEN_USER*>(user)->User.Sid;
  PSID container_sid =
      reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(container)->TokenAppContainer;
  if (!user_sid || !container_sid || !IsValidSid(user_sid) || !IsValidSid(container_sid) ||
      EqualSid(owner, user_sid) || EqualSid(owner, container_sid) || EqualSid(user_sid, container_sid)) {
    return ERROR_ACCESS_DENIED;
  }
  PSID expected[] = {owner, user_sid, container_sid};
  const DWORD expected_access[] = {FILE_ALL_ACCESS, wire::kPipeClientAccess, wire::kPipeClientAccess};
  bool seen[3]{};
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    if (!GetAce(dacl, i, &raw)) return GetLastError();
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
        ace->Header.AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8) return ERROR_ACCESS_DENIED;
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (!IsValidSid(sid) ||
        offsetof(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(sid) != ace->Header.AceSize) {
      return ERROR_ACCESS_DENIED;
    }
    size_t match = 0;
    while (match < 3 && !EqualSid(sid, expected[match])) ++match;
    if (match == 3 || seen[match] || ace->Mask != expected_access[match]) return ERROR_ACCESS_DENIED;
    seen[match] = true;
  }
  void* raw_label = nullptr;
  if (!GetAce(label, 0, &raw_label)) return GetLastError();
  const auto* mandatory = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(raw_label);
  PSID integrity = const_cast<DWORD*>(&mandatory->SidStart);
  return mandatory->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE &&
         mandatory->Header.AceFlags == 0 &&
         mandatory->Header.AceSize == offsetof(SYSTEM_MANDATORY_LABEL_ACE, SidStart) +
                                      GetSidLengthRequired(1) &&
         mandatory->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP &&
         IsWellKnownSid(integrity, WinLowLabelSid)
      ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

}  // namespace

DWORD ProtectedBootstrap::Transfer(void* data, DWORD size, bool sending, DWORD timeout) {
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return GetLastError();
  auto* cursor = static_cast<char*>(data);
  const uint64_t deadline = GetTickCount64() + timeout;
  while (size != 0) {
    if (!ResetEvent(event.get())) return GetLastError();
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL ok = sending ? WriteFile(pipe_, cursor, size, nullptr, &operation)
                            : ReadFile(pipe_, cursor, size, nullptr, &operation);
    if (!ok) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) return error;
      const uint64_t now = GetTickCount64();
      const DWORD remaining = timeout == INFINITE ? kAuthorityPollInterval :
          now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
      DWORD wait = WaitForSingleObject(event.get(), remaining);
      DWORD authority_error = ERROR_SUCCESS;
      // An idle endpoint must stop admitting peers when SCM retires the
      // authority. Keep its pending read until either the bytes or that
      // liveness failure arrive, including when a frame is partially read.
      while (timeout == INFINITE && wait == WAIT_TIMEOUT) {
        authority_error = CheckLiveAuthority();
        if (authority_error != ERROR_SUCCESS) break;
        wait = WaitForSingleObject(event.get(), kAuthorityPollInterval);
      }
      if (wait != WAIT_OBJECT_0) {
        const DWORD status = authority_error != ERROR_SUCCESS ? authority_error :
            wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        CancelIoEx(pipe_, &operation);
        GetOverlappedResult(pipe_, &operation, &transferred, TRUE);
        return status;
      }
    }
    if (!GetOverlappedResult(pipe_, &operation, &transferred, FALSE)) return GetLastError();
    if (transferred == 0 || transferred > size) return ERROR_BROKEN_PIPE;
    cursor += transferred;
    size -= transferred;
  }
  return ERROR_SUCCESS;
}

DWORD ProtectedBootstrap::Reply(uint64_t id, DWORD status) {
  wire::Reply reply{wire::kMagic, wire::kVersion, status, 0, id};
  return Transfer(&reply, sizeof(reply), true, kReplyTimeout);
}

namespace {

bool ValidRegistration(const wire::Request& request) {
  if (request.magic != wire::kMagic || request.version != wire::kVersion ||
      request.registration_id == 0) return false;
  if (request.operation == wire::Operation::kRevoke) {
    return request.uid == 0 && request.process_handle == 0;
  }
  return request.operation == wire::Operation::kRegister &&
         request.uid <= std::numeric_limits<int32_t>::max() &&
         request.process_handle != 0 && request.process_handle <= INTPTR_MAX;
}

struct Registrations {
  ProtectedPeers& peers;
  std::map<uint64_t, ProtectedPeers::Peer> entries;
  ~Registrations() { for (const auto& [id, peer] : entries) peers.Revoke(peer); }

  DWORD Apply(const wire::Request& request) {
    if (request.operation == wire::Operation::kRevoke) {
      const auto it = entries.find(request.registration_id);
      if (it == entries.end()) return ERROR_NOT_FOUND;
      peers.Revoke(it->second);
      entries.erase(it);
      return ERROR_SUCCESS;
    }
    // A complete authentic frame transfers ownership, even if admission fails.
    const Handle transferred(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.process_handle)));
    if (entries.contains(request.registration_id)) return ERROR_ALREADY_EXISTS;
    if (entries.size() >= 4096) {
      std::erase_if(entries, [](const auto& entry) { return !entry.second->live(); });
      if (entries.size() >= 4096) return ERROR_TOO_MANY_OPEN_FILES;
    }
    // Allocate bookkeeping before granting the peer admission.
    auto slot = entries.emplace(request.registration_id, ProtectedPeers::Peer{}).first;
    const DWORD status = peers.Register(transferred.get(), request.uid, &slot->second);
    if (status != ERROR_SUCCESS) entries.erase(slot);
    return status;
  }
};

}  // namespace

ProtectedBootstrap::~ProtectedBootstrap() {
  if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
  if (service_) CloseServiceHandle(service_);
}

DWORD ProtectedBootstrap::Connect(std::wstring_view locator,
                                  std::unique_ptr<ProtectedBootstrap>* result) try {
  if (!result || *result || !ValidLocator(locator)) return ERROR_INVALID_PARAMETER;
  std::unique_ptr<ProtectedBootstrap> session(new ProtectedBootstrap);
  session->pipe_ = CreateFileW(std::wstring(locator).c_str(), wire::kPipeClientAccess,
      0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (session->pipe_ == INVALID_HANDLE_VALUE) return GetLastError();
  DWORD flags = 0;
  if (!GetNamedPipeInfo(session->pipe_, &flags, nullptr, nullptr, nullptr)) return GetLastError();
  if (flags & (PIPE_SERVER_END | PIPE_TYPE_MESSAGE)) return ERROR_INVALID_HANDLE;
  DWORD status = CheckPipeSecurity(session->pipe_);
  if (status != ERROR_SUCCESS) return status;
  const Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) return GetLastError();
  session->service_ = OpenServiceW(manager.get(), wire::kAuthorityService,
                                   SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
  if (!session->service_) return GetLastError();
  status = CheckServiceConfig(session->service_);
  if (status != ERROR_SUCCESS) return status;
  if (!GetNamedPipeServerProcessId(session->pipe_, &session->server_pid_)) return GetLastError();
  status = session->Handshake();
  if (status != ERROR_SUCCESS) return status;
  *result = std::move(session);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

DWORD ProtectedBootstrap::CheckLiveAuthority() const {
#ifdef GESHER_BINDER_BOOTSTRAP_FIXTURE
  if (!service_) return ERROR_SUCCESS;
#endif
  ULONG pid = 0, session = 0;
  SERVICE_STATUS_PROCESS report{};
  DWORD bytes = 0;
  if (!GetNamedPipeServerProcessId(pipe_, &pid) || !GetNamedPipeServerSessionId(pipe_, &session) ||
      !QueryServiceStatusEx(service_, SC_STATUS_PROCESS_INFO,
                           reinterpret_cast<BYTE*>(&report), sizeof(report), &bytes)) {
    return GetLastError();
  }
  return session == 0 && pid != 0 && pid == server_pid_ && pid == report.dwProcessId &&
      report.dwCurrentState == SERVICE_RUNNING &&
      (report.dwServiceType & ~SERVICE_INTERACTIVE_PROCESS) == SERVICE_WIN32_OWN_PROCESS
      ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

DWORD ProtectedBootstrap::Handshake() {
  DWORD status = CheckLiveAuthority();
  if (status != ERROR_SUCCESS) return status;
  wire::Hello hello{wire::kMagic, wire::kVersion, wire::Operation::kHello, 0};
  status = Transfer(&hello, sizeof(hello), true, kHandshakeTimeout);
  if (status != ERROR_SUCCESS) return status;
  wire::Request ready{};
  status = Transfer(&ready, sizeof(ready), false, kHandshakeTimeout);
  if (status != ERROR_SUCCESS) return status;
  status = CheckLiveAuthority();
  if (status != ERROR_SUCCESS) return status;
  if (ready.magic != wire::kMagic || ready.version != wire::kVersion ||
      ready.operation != wire::Operation::kReady || ready.uid != 0 ||
      ready.registration_id != 0 || ready.process_handle != 0) return ERROR_INVALID_DATA;
  return Reply(0, ERROR_SUCCESS);
}

DWORD ProtectedBootstrap::Serve(ProtectedPeers& peers) {
  if (pipe_ == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
  DWORD result = ERROR_SUCCESS;
  try {
    Registrations registrations{peers, {}};
    for (;;) {
      wire::Request request{};
      result = CheckLiveAuthority();
      if (result != ERROR_SUCCESS) break;
      result = Transfer(&request, sizeof(request), false, INFINITE);
      if (result != ERROR_SUCCESS) break;
      result = CheckLiveAuthority();
      if (result != ERROR_SUCCESS) break;
      if (!ValidRegistration(request)) {
        result = ERROR_INVALID_DATA;
        break;
      }
      result = Reply(request.registration_id, registrations.Apply(request));
      if (result != ERROR_SUCCESS) break;
    }
  } catch (const std::bad_alloc&) {
    result = ERROR_NOT_ENOUGH_MEMORY;
  }
  CloseHandle(pipe_);
  pipe_ = INVALID_HANDLE_VALUE;
  return result;
}

#ifdef GESHER_BINDER_BOOTSTRAP_FIXTURE
DWORD ProtectedBootstrap::AdoptFixturePipe(HANDLE connected_pipe,
                                           std::unique_ptr<ProtectedBootstrap>* result) {
  if (connected_pipe == INVALID_HANDLE_VALUE || !connected_pipe || !result || *result) {
    return ERROR_INVALID_PARAMETER;
  }
  const Handle supplied(connected_pipe);
  std::unique_ptr<ProtectedBootstrap> session(new (std::nothrow) ProtectedBootstrap);
  if (!session) return ERROR_NOT_ENOUGH_MEMORY;
  if (!DuplicateHandle(GetCurrentProcess(), supplied.get(), GetCurrentProcess(), &session->pipe_,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) return GetLastError();
  const DWORD status = session->Handshake();
  if (status == ERROR_SUCCESS) *result = std::move(session);
  return status;
}
#endif

}  // namespace gesher::binder
