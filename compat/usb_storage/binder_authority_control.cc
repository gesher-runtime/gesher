// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "binder_authority_control.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <vector>

#include "../binder/protected_bootstrap_wire.h"
#include "authority_launch.h"
#include "authority_peer.h"
#include "binder_listener_factory.h"

namespace gesher::usb_storage {
namespace {

namespace wire = gesher::binder::bootstrap_wire;
constexpr DWORD kHandshakeWait = 15000;
constexpr DWORD kTransferRights =
    PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL;

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
struct LocalFreeOwner {
  void operator()(void* allocation) const { LocalFree(allocation); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;
using LocalMemory = std::unique_ptr<void, LocalFreeOwner>;

bool ValidLocator(std::wstring_view path) {
  constexpr std::wstring_view prefix(wire::kPipePrefix);
  if (!path.starts_with(prefix) || path.size() != prefix.size() + wire::kLocatorHexDigits) {
    return false;
  }
  for (const wchar_t digit : path.substr(prefix.size())) {
    if (!((digit >= L'0' && digit <= L'9') || (digit >= L'a' && digit <= L'f'))) return false;
  }
  return true;
}

DWORD ConnectionSecurity(HANDLE process, LocalMemory* descriptor, LocalMemory* package_sid) {
  HANDLE found = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &found)) return GetLastError();
  Handle token(found);
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  alignas(TOKEN_APPCONTAINER_INFORMATION)
      BYTE container[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE]{};
  DWORD size = 0;
  if (!GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &size) ||
      !GetTokenInformation(token.get(), TokenAppContainerSid, container, sizeof(container), &size)) {
    return GetLastError();
  }
  const PSID owner = reinterpret_cast<const TOKEN_USER*>(user)->User.Sid;
  const PSID package =
      reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(container)->TokenAppContainer;
  if (!owner || !package || !IsValidSid(owner) || !IsValidSid(package) || EqualSid(owner, package)) {
    return ERROR_INVALID_SID;
  }
  package_sid->reset(LocalAlloc(LMEM_FIXED, GetLengthSid(package)));
  if (!*package_sid) return ERROR_NOT_ENOUGH_MEMORY;
  if (!CopySid(GetLengthSid(package), package_sid->get(), package)) return GetLastError();

  LPWSTR owner_text = nullptr, package_text = nullptr;
  if (!ConvertSidToStringSidW(owner, &owner_text)) return GetLastError();
  const LocalMemory release_owner(owner_text);
  if (!ConvertSidToStringSidW(package, &package_text)) return GetLastError();
  const LocalMemory release_package(package_text);

  // Generic write includes FILE_CREATE_PIPE_INSTANCE, so give both token
  // principals only the byte-stream client rights.
  static_assert(wire::kPipeClientAccess == 0x0012018b);
  const std::wstring policy = std::wstring(L"O:SYG:SYD:P(A;;FA;;;SY)(A;;0x12018b;;;") +
      owner_text + L")(A;;0x12018b;;;" + package_text + L")S:(ML;;NW;;;LW)";
  PSECURITY_DESCRIPTOR converted = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          policy.c_str(), SDDL_REVISION_1, &converted, nullptr)) return GetLastError();
  descriptor->reset(converted);
  return ERROR_SUCCESS;
}

DWORD WaitIo(HANDLE pipe, OVERLAPPED* pending, HANDLE stop, HANDLE broker,
             DWORD milliseconds, DWORD* bytes) {
  const HANDLE waits[] = {stop, broker, pending->hEvent};
  const DWORD state = WaitForMultipleObjects(3, waits, FALSE, milliseconds);
  if (state == WAIT_OBJECT_0 + 2) {
    return GetOverlappedResult(pipe, pending, bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
  }
  DWORD error = ERROR_OPERATION_ABORTED;
  if (state == WAIT_OBJECT_0 + 1) error = ERROR_PROCESS_ABORTED;
  if (state == WAIT_TIMEOUT) error = ERROR_TIMEOUT;
  if (state == WAIT_FAILED) error = GetLastError();
  CancelIoEx(pipe, pending);
  // Cancel must retire the OVERLAPPED and its buffer before we return.
  GetOverlappedResult(pipe, pending, bytes, TRUE);
  return error;
}

}  // namespace

// SCM's default Authenticated Users ACE admits the desktop token but does not
// satisfy the lowbox access check. The installed service grants its one active
// verified broker package only enough access to authenticate the SCM peer.
class BinderServiceQueryGrant {
 public:
  explicit BinderServiceQueryGrant(LocalMemory package)
      : scope_(ExclusiveSlot(), std::try_to_lock), package_(std::move(package)) {}
  ~BinderServiceQueryGrant() {
    if (applied_) Retire();
    if (service_) CloseServiceHandle(service_);
  }

  DWORD Apply() {
    if (!scope_.owns_lock()) return ERROR_BUSY;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return GetLastError();
    service_ = OpenServiceW(manager, wire::kAuthorityService,
                            SERVICE_QUERY_STATUS | READ_CONTROL | WRITE_DAC);
    const DWORD open_status = service_ ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(manager);
    if (open_status != ERROR_SUCCESS) return open_status;
    SERVICE_STATUS_PROCESS state{};
    DWORD size = 0;
    if (!QueryServiceStatusEx(service_, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&state),
                              sizeof(state), &size)) return GetLastError();
    if (state.dwServiceType != SERVICE_WIN32_OWN_PROCESS || state.dwCurrentState != SERVICE_RUNNING ||
        state.dwProcessId != GetCurrentProcessId()) return ERROR_ACCESS_DENIED;

    std::vector<BYTE> security;
    PACL dacl = nullptr;
    DWORD status = ReadAcl(&security, &dacl);
    if (status != ERROR_SUCCESS) return status;
    DWORD index = MAXDWORD;
    status = FindAce(dacl, &index);
    if (status != ERROR_SUCCESS) return status;
    if (index == MAXDWORD) {
      EXPLICIT_ACCESSW permission{};
      permission.grfAccessPermissions = kQuery;
      permission.grfAccessMode = GRANT_ACCESS;
      permission.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      permission.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
      permission.Trustee.ptstrName = static_cast<wchar_t*>(package_.get());
      PACL updated = nullptr;
      status = SetEntriesInAclW(1, &permission, dacl, &updated);
      if (status != ERROR_SUCCESS) return status;
      const LocalMemory release_acl(updated);
      SECURITY_DESCRIPTOR replacement{};
      if (!InitializeSecurityDescriptor(&replacement, SECURITY_DESCRIPTOR_REVISION) ||
          !SetSecurityDescriptorDacl(&replacement, TRUE, updated, FALSE) ||
          !SetSecurityDescriptorControl(&replacement, SE_DACL_PROTECTED, SE_DACL_PROTECTED) ||
          !SetServiceObjectSecurity(service_, DACL_SECURITY_INFORMATION, &replacement)) {
        return GetLastError();
      }
    }
    // A stale exact read-only ACE from a stopped instance may be retired here.
    applied_ = true;
    return ERROR_SUCCESS;
  }

  DWORD Retire() {
    if (!applied_) return ERROR_SUCCESS;
    std::vector<BYTE> security;
    PACL dacl = nullptr;
    DWORD status = ReadAcl(&security, &dacl);
    if (status != ERROR_SUCCESS) return status;
    DWORD index = MAXDWORD;
    status = FindAce(dacl, &index);
    if (status != ERROR_SUCCESS) return status;
    if (index != MAXDWORD) {
      if (!DeleteAce(dacl, index) ||
          !SetServiceObjectSecurity(service_, DACL_SECURITY_INFORMATION, security.data())) {
        return GetLastError();
      }
      status = ReadAcl(&security, &dacl);
      if (status != ERROR_SUCCESS) return status;
      index = MAXDWORD;
      status = FindAce(dacl, &index);
      if (status != ERROR_SUCCESS) return status;
      if (index != MAXDWORD) return ERROR_INVALID_ACL;
    }
    applied_ = false;
    return ERROR_SUCCESS;
  }

 private:
  static constexpr DWORD kQuery = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS;
  static std::mutex& ExclusiveSlot() {
    static std::mutex slot;
    return slot;
  }

  DWORD ReadAcl(std::vector<BYTE>* security, PACL* dacl) const {
    DWORD size = 0;
    if (QueryServiceObjectSecurity(service_, DACL_SECURITY_INFORMATION, nullptr, 0, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 64 * 1024) {
      return ERROR_INVALID_DATA;
    }
    security->resize(size);
    if (!QueryServiceObjectSecurity(
            service_, DACL_SECURITY_INFORMATION, security->data(), size, &size)) return GetLastError();
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(security->data(), &present, dacl, &defaulted) ||
        !present || !*dacl || !IsValidAcl(*dacl)) return ERROR_INVALID_ACL;
    return ERROR_SUCCESS;
  }

  DWORD FindAce(PACL dacl, DWORD* index) const {
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
      void* entry = nullptr;
      if (!GetAce(dacl, i, &entry)) return GetLastError();
      const auto* header = static_cast<const ACE_HEADER*>(entry);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
      auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
      if (!EqualSid(&ace->SidStart, package_.get())) continue;
      if (ace->Mask != kQuery || ace->Header.AceFlags != 0 || *index != MAXDWORD) {
        return ERROR_INVALID_ACL;
      }
      *index = i;
    }
    return ERROR_SUCCESS;
  }

  std::unique_lock<std::mutex> scope_;
  LocalMemory package_;
  SC_HANDLE service_ = nullptr;
  bool applied_ = false;
};

BinderAuthorityControl::~BinderAuthorityControl() {
  peer_.reset();
  if (broker_) CloseHandle(broker_);
}

DWORD BinderAuthorityControl::CreateLocator(std::wstring* locator) {
  if (!locator || !locator->empty()) return ERROR_INVALID_PARAMETER;
  constexpr wchar_t hex[] = L"0123456789abcdef";
  BYTE nonce[wire::kLocatorHexDigits / 2]{};
  if (BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
    return ERROR_GEN_FAILURE;
  }
  locator->assign(wire::kPipePrefix);
  for (BYTE value : nonce) {
    locator->push_back(hex[value >> 4]);
    locator->push_back(hex[value & 15]);
  }
  return ERROR_SUCCESS;
}

DWORD BinderAuthorityControl::Connect(AuthorityLaunch& broker,
    const std::wstring& registration_locator, const std::wstring& listener_locator, HANDLE stop,
    std::unique_ptr<BinderAuthorityControl>* registration,
    std::unique_ptr<BinderAuthorityControl>* listeners, std::optional<DWORD>* broker_exit) {
  if (broker_exit) broker_exit->reset();
  if (!registration || *registration || !listeners || *listeners || registration == listeners ||
      !stop || stop == INVALID_HANDLE_VALUE || !ValidLocator(registration_locator) ||
      !ValidLocator(listener_locator) || registration_locator == listener_locator) {
    return ERROR_INVALID_PARAMETER;
  }
  LocalMemory descriptor, package;
  DWORD status = ConnectionSecurity(broker.process(), &descriptor, &package);
  if (status != ERROR_SUCCESS) return status;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};
  std::unique_ptr<BinderAuthorityControl> admission, factory;
  status = Prepare(broker, registration_locator, stop, wire::Role::kRegistration, security,
                   &admission);
  if (status != ERROR_SUCCESS) return status;
  status = Prepare(broker, listener_locator, stop, wire::Role::kListeners, security, &factory);
  if (status != ERROR_SUCCESS) return status;
  BinderServiceQueryGrant service_query(std::move(package));
  status = service_query.Apply();
  if (status != ERROR_SUCCESS) return status;
  if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) return ERROR_OPERATION_ABORTED;
  status = broker.Resume();
  if (status == ERROR_SUCCESS) status = admission->Handshake();
  if (status == ERROR_SUCCESS) status = factory->Handshake();
  // Each channel retains its own authenticated SCM handle. No new AppContainer
  // connection needs query access after both handshakes have finished.
  if (status == ERROR_SUCCESS) status = service_query.Retire();
  if (status != ERROR_SUCCESS) {
    // Observe only an already exited child; FailClosed can itself terminate it.
    DWORD exit_code = 0;
    if (broker_exit && WaitForSingleObject(broker.process(), 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(broker.process(), &exit_code)) {
      *broker_exit = exit_code;
    }
    return admission->FailClosed(status);
  }
  *registration = std::move(admission);
  *listeners = std::move(factory);
  return ERROR_SUCCESS;
}

DWORD BinderAuthorityControl::Prepare(AuthorityLaunch& broker, const std::wstring& locator,
                                       HANDLE stop, wire::Role role, SECURITY_ATTRIBUTES& security,
                                       std::unique_ptr<BinderAuthorityControl>* result) {
  auto control = std::unique_ptr<BinderAuthorityControl>(new (std::nothrow) BinderAuthorityControl);
  if (!control) return ERROR_NOT_ENOUGH_MEMORY;
  control->stop_ = stop;
  control->broker_pid_ = broker.pid();
  control->role_ = role;
  HANDLE own = GetCurrentProcess();
  if (!DuplicateHandle(own, broker.process(), own, &control->broker_,
                       PROCESS_TERMINATE | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, 0)) {
    return GetLastError();
  }
  const DWORD status = AuthorityPeer::Create(broker.process(), locator.c_str(), &security,
                                             &control->peer_);
  if (status != ERROR_SUCCESS) return status;
  *result = std::move(control);
  return ERROR_SUCCESS;
}

DWORD BinderAuthorityControl::Handshake() {
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return GetLastError();
  if (WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) return ERROR_OPERATION_ABORTED;
  OVERLAPPED pending{};
  pending.hEvent = event.get();
  const HANDLE pipe = peer_->pipe();
  if (!ConnectNamedPipe(pipe, &pending)) {
    DWORD status = GetLastError();
    if (status == ERROR_IO_PENDING) {
      DWORD bytes = 0;
      status = WaitIo(pipe, &pending, stop_, broker_, kHandshakeWait, &bytes);
    } else if (status == ERROR_PIPE_CONNECTED) {
      status = ERROR_SUCCESS;
    }
    if (status != ERROR_SUCCESS) return status;
  }
  wire::Hello hello{};
  DWORD status = Transfer(&hello, sizeof(hello), false);
  if (status != ERROR_SUCCESS) return status;
  if (hello.magic != wire::kMagic || hello.version != wire::kVersion ||
      hello.operation != wire::Operation::kHello || hello.role != role_) {
    return ERROR_INVALID_DATA;
  }
  status = peer_->Authenticate();
  if (status != ERROR_SUCCESS) return status;
  return Exchange(wire::Operation::kReady, 0, 0, binder::PeerRole::kClient, nullptr);
}

DWORD BinderAuthorityControl::Transfer(void* data, DWORD bytes, bool writing, bool wait_idle) {
  if (!peer_) return ERROR_INVALID_HANDLE;
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return GetLastError();
  auto* buffer = static_cast<std::byte*>(data);
  const HANDLE pipe = peer_->pipe();
  const ULONGLONG deadline = GetTickCount64() + kHandshakeWait;
  while (bytes != 0) {
    if (WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) return ERROR_OPERATION_ABORTED;
    const ULONGLONG now = GetTickCount64();
    if (!wait_idle && now >= deadline) return ERROR_TIMEOUT;
    if (!ResetEvent(event.get())) return GetLastError();
    OVERLAPPED pending{};
    pending.hEvent = event.get();
    DWORD count = 0;
    const BOOL completed = writing ? WriteFile(pipe, buffer, bytes, nullptr, &pending)
                                   : ReadFile(pipe, buffer, bytes, nullptr, &pending);
    if (completed) {
      if (!GetOverlappedResult(pipe, &pending, &count, FALSE)) return GetLastError();
    } else {
      DWORD status = GetLastError();
      if (status != ERROR_IO_PENDING) return status;
      const DWORD remaining = wait_idle ? INFINITE : static_cast<DWORD>(deadline - now);
      status = WaitIo(pipe, &pending, stop_, broker_, remaining, &count);
      if (status != ERROR_SUCCESS) return status;
    }
    if (!count || count > bytes) return ERROR_BROKEN_PIPE;
    buffer += count;
    bytes -= count;
  }
  return ERROR_SUCCESS;
}

DWORD BinderAuthorityControl::FailClosed(DWORD status) {
  peer_.reset();
  // No numeric remote handle can be closed after a send starts. The broker
  // may already have consumed that value. Retire the exact process instead.
  if (broker_ && WaitForSingleObject(broker_, 0) == WAIT_TIMEOUT) {
    TerminateProcess(broker_, ERROR_OPERATION_ABORTED);
  }
  return status;
}

DWORD BinderAuthorityControl::Exchange(wire::Operation op, uint64_t id, uint32_t uid,
                                       binder::PeerRole role, HANDLE remote_handle) {
  wire::Request request{wire::kMagic, wire::kVersion, op, uid, id,
                        reinterpret_cast<uintptr_t>(remote_handle), role, 0};
  DWORD status = Transfer(&request, sizeof(request), true);
  if (status != ERROR_SUCCESS) return FailClosed(status);
  wire::Reply reply{};
  status = Transfer(&reply, sizeof(reply), false);
  if (status != ERROR_SUCCESS) return FailClosed(status);
  status = peer_->Authenticate();
  if (status != ERROR_SUCCESS) return FailClosed(status);
  if (reply.magic != wire::kMagic || reply.version != wire::kVersion ||
      reply.reserved != 0 || reply.registration_id != id) return FailClosed(ERROR_INVALID_DATA);
  return reply.status;
}

DWORD BinderAuthorityControl::Register(uint64_t id, uint32_t uid, binder::PeerRole role,
                                       const AuthorityLaunch& process) {
  if (id == 0 || uid > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      !binder::IsAllowedPeerRole(uid, role)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (!peer_ || role_ != wire::Role::kRegistration) return ERROR_INVALID_HANDLE;
  DWORD status = peer_->Authenticate();
  if (status != ERROR_SUCCESS) return FailClosed(status);
  HANDLE remote = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), process.process(), broker_, &remote,
                       kTransferRights, FALSE, 0)) return GetLastError();
  return Exchange(wire::Operation::kRegister, id, uid, role, remote);
}

DWORD BinderAuthorityControl::Revoke(uint64_t id) {
  if (!id) return ERROR_INVALID_PARAMETER;
  if (!peer_ || role_ != wire::Role::kRegistration) return ERROR_INVALID_HANDLE;
  const DWORD status = peer_->Authenticate();
  if (status != ERROR_SUCCESS) return FailClosed(status);
  return Exchange(wire::Operation::kRevoke, id, 0, binder::PeerRole::kClient, nullptr);
}

DWORD BinderAuthorityControl::ServeListeners(BinderListenerFactory& factory) {
  if (!peer_ || role_ != wire::Role::kListeners || factory.broker_pid() != broker_pid_) {
    return ERROR_INVALID_PARAMETER;
  }
  const auto seal = [&](DWORD error) {
    factory.Revoke();
    return FailClosed(error);
  };
  for (;;) {
    wire::ListenerRequest request{};
    auto* bytes = reinterpret_cast<std::byte*>(&request);
    // An idle broker may wait for its next client indefinitely. Bound the
    // remainder of the frame as soon as it begins a listener request.
    DWORD status = Transfer(bytes, 1, false, true);
    if (status == ERROR_SUCCESS) status = Transfer(bytes + 1, sizeof(request) - 1, false);
    if (status != ERROR_SUCCESS) return seal(status);
    if (request.magic != wire::kMagic || request.version != wire::kVersion ||
        request.operation != wire::Operation::kTakeListener || request.reserved != 0) {
      return seal(ERROR_INVALID_DATA);
    }
    status = peer_->Authenticate();
    if (status != ERROR_SUCCESS) return seal(status);
    HANDLE listener = nullptr;
    const DWORD issued = factory.Take(&listener);
    wire::ListenerReply reply{wire::kMagic, wire::kVersion, issued, 0,
                              reinterpret_cast<uintptr_t>(listener)};
    status = Transfer(&reply, sizeof(reply), true);
    if (status != ERROR_SUCCESS) return seal(status);
    if (issued != ERROR_SUCCESS) return seal(issued);
  }
}

}  // namespace gesher::usb_storage
