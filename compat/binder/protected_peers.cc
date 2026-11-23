// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_peers.h"

#include <aclapi.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace gesher::binder {
namespace {

DWORD LiveStatus(HANDLE process) {
  const DWORD wait = WaitForSingleObject(process, 0);
  if (wait == WAIT_TIMEOUT) return ERROR_SUCCESS;
  return wait == WAIT_FAILED ? GetLastError() : ERROR_PROCESS_ABORTED;
}

struct FreeSecurity {
  void operator()(void* value) const { LocalFree(value); }
};

DWORD CheckObjectSecurity(HANDLE process, PSID authority) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD error = GetSecurityInfo(process, SE_KERNEL_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl,
      nullptr, &descriptor);
  if (error != ERROR_SUCCESS) return error;
  const std::unique_ptr<void, FreeSecurity> free(descriptor);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!owner || !IsValidSid(owner) || !EqualSid(owner, authority) || !dacl ||
      !GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      !(control & SE_DACL_PROTECTED) || !IsValidAcl(dacl) || dacl->AceCount != 1) {
    return ERROR_ACCESS_DENIED;
  }
  void* entry = nullptr;
  if (!GetAce(dacl, 0, &entry)) return GetLastError();
  const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
  if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
      ace->Header.AceSize < offsetof(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(authority) ||
      ace->Mask != PROCESS_ALL_ACCESS) {
    return ERROR_ACCESS_DENIED;
  }
  PSID granted = const_cast<DWORD*>(&ace->SidStart);
  return IsValidSid(granted) && EqualSid(granted, authority) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

}  // namespace

ProtectedPeer::~ProtectedPeer() {
  if (process_) CloseHandle(process_);
}

bool ProtectedPeer::live() const {
  return !revoked_.load(std::memory_order_acquire) && LiveStatus(process_) == ERROR_SUCCESS;
}

DWORD ProtectedPeers::Register(HANDLE launched_process, uint32_t android_uid, Peer* result) {
  alignas(DWORD) std::byte system[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(system);
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) return GetLastError();
  return RegisterOwned(launched_process, android_uid, system, result);
}

#ifdef GESHER_BINDER_PEER_FIXTURE
DWORD ProtectedPeers::RegisterForFixture(HANDLE launched_process, uint32_t android_uid,
                                         PSID fixture_owner, Peer* result) {
  return RegisterOwned(launched_process, android_uid, fixture_owner, result);
}
#endif

DWORD ProtectedPeers::RegisterOwned(HANDLE launched_process, uint32_t android_uid,
                                   PSID owner, Peer* result) try {
  if (!launched_process || launched_process == INVALID_HANDLE_VALUE || !result || *result ||
      !owner || !IsValidSid(owner) || android_uid > std::numeric_limits<int32_t>::max()) {
    return ERROR_INVALID_PARAMETER;
  }
  auto peer = std::shared_ptr<ProtectedPeer>(new ProtectedPeer);
  if (!DuplicateHandle(GetCurrentProcess(), launched_process, GetCurrentProcess(), &peer->process_,
                       PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                       FALSE, 0)) return GetLastError();
  peer->pid_ = GetProcessId(peer->process_);
  if (!peer->pid_) return GetLastError();
  DWORD error = CheckObjectSecurity(launched_process, owner);
  if (error != ERROR_SUCCESS) return error;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(peer->process_, &created, &exited, &kernel, &user)) return GetLastError();
  peer->creation_time_ = (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
  // The retained live process object anchored this PID. Get the OS session
  // without asking a protected lowbox broker to open another process token.
  if (!ProcessIdToSessionId(peer->pid_, &peer->session_)) return GetLastError();
  peer->uid_ = android_uid;

  std::lock_guard lock(mutex_);
  error = LiveStatus(peer->process_);
  if (error != ERROR_SUCCESS) return error;
  std::erase_if(peers_, [](const auto& entry) {
    return WaitForSingleObject(entry.second->process(), 0) == WAIT_OBJECT_0;
  });
  if (!peers_.emplace(peer->pid_, peer).second) return ERROR_ALREADY_EXISTS;
  *result = std::move(peer);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

DWORD ProtectedPeers::FindClient(HANDLE server_pipe, Peer* result) const {
  if (!server_pipe || server_pipe == INVALID_HANDLE_VALUE || !result || *result) {
    return ERROR_INVALID_PARAMETER;
  }
  DWORD flags = 0;
  if (!GetNamedPipeInfo(server_pipe, &flags, nullptr, nullptr, nullptr)) return GetLastError();
  if (!(flags & PIPE_SERVER_END)) return ERROR_INVALID_HANDLE;
  ULONG pid = 0, session = 0;
  if (!GetNamedPipeClientProcessId(server_pipe, &pid) ||
      !GetNamedPipeClientSessionId(server_pipe, &session)) return GetLastError();
  std::lock_guard lock(mutex_);
  const auto it = peers_.find(pid);
  if (it == peers_.end() || it->second->session() != session || !it->second->live()) {
    return ERROR_ACCESS_DENIED;
  }
  *result = it->second;
  return ERROR_SUCCESS;
}

void ProtectedPeers::Revoke(const Peer& peer) {
  if (!peer) return;
  std::lock_guard lock(mutex_);
  const auto it = peers_.find(peer->pid());
  if (it != peers_.end() && it->second == peer) {
    it->second->revoked_.store(true, std::memory_order_release);
  }
}

}  // namespace gesher::binder
