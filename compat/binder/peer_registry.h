// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <array>
#include <map>
#include <memory>
#include <mutex>

#include "../posix/include/gesher/windows_process_identity.h"

namespace gesher::binder_peer {

enum class Role : uint32_t { kLauncher = 1, kServiceManager, kSystemServer, kApplication };

struct HandleCloser {
  void operator()(void* handle) const {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
using OwnedHandle = std::unique_ptr<void, HandleCloser>;

struct Peer {
  const OwnedHandle process;
  const process_identity::Process generation;
  const Role role;
  const uint32_t uid;
  const std::array<uint8_t, 16> installation;
};

class Registry {
 public:
  using Lease = std::shared_ptr<const Peer>;

  explicit Registry(process_identity::Principal authority) : authority_(authority) {}

  // Only the broker's private launcher control channel may call this entry.
  // A public pipe connection cannot enroll or assign a role to a process.
  bool Enroll(OwnedHandle process, DWORD pid, uint64_t birth, Role role, uint32_t uid,
              const std::array<uint8_t, 16>& installation, PSID container) {
    process_identity::Process generation;
    if (!process_identity::ReadLiveProcess(process.get(), &generation) ||
        generation.pid != pid || generation.birth != birth ||
        !Validate(generation.principal, role, uid, installation, container)) {
      return false;
    }
    auto peer = std::shared_ptr<Peer>(new Peer{
        std::move(process), generation, role, uid, installation});
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return false;
    PruneExitedLocked();
    if (peers_.size() >= 4096 || peers_.find(generation.pid) != peers_.end()) return false;
    for (const auto& entry : peers_) {
      if (role != Role::kApplication && entry.second->role == role) return false;
    }
    return peers_.emplace(generation.pid, std::move(peer)).second;
  }

  // Use only after reading the request from the kernel's server-side pipe.
  Lease Authenticate(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) return {};
    Lease peer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = peers_.find(pid);
      if (!active_ || found == peers_.end()) return {};
      peer = found->second;
    }
    return process_identity::MatchPipeSender(pipe, peer->process.get(), peer->generation) &&
               Live(peer) ? peer : Lease{};
  }

  bool Live(const Lease& peer) {
    if (!peer) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = peers_.find(peer->generation.pid);
    return active_ && found != peers_.end() && found->second == peer &&
           WaitForSingleObject(peer->process.get(), 0) == WAIT_TIMEOUT;
  }

  Lease FindGeneration(DWORD pid, uint64_t birth, Role role) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = peers_.find(pid);
    if (!active_ || found == peers_.end() || found->second->generation.birth != birth ||
        found->second->role != role ||
        WaitForSingleObject(found->second->process.get(), 0) != WAIT_TIMEOUT) {
      return {};
    }
    return found->second;
  }

  bool Retire(DWORD pid, uint64_t birth) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = peers_.find(pid);
    if (found == peers_.end() || found->second->generation.birth != birth) return false;
    peers_.erase(found);
    return true;
  }

  void RevokeInstallation(const std::array<uint8_t, 16>& installation) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto peer = peers_.begin(); peer != peers_.end();) {
      if (peer->second->role == Role::kApplication && peer->second->installation == installation) {
        peer = peers_.erase(peer);
      } else {
        ++peer;
      }
    }
  }

  void CloseAuthority() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    peers_.clear();
  }

 private:
  bool Validate(const process_identity::Principal& principal, Role role, uint32_t uid,
                const std::array<uint8_t, 16>& installation, PSID container) const {
    if (!(principal.account == authority_.account) || principal.session != authority_.session ||
        principal.logon.LowPart != authority_.logon.LowPart ||
        principal.logon.HighPart != authority_.logon.HighPart) {
      return false;
    }
    if (role == Role::kApplication) {
      return uid >= 10000 && uid <= 19999 && principal.container.Matches(container) &&
             principal.integrity < SECURITY_MANDATORY_MEDIUM_RID &&
             installation != std::array<uint8_t, 16>{};
    }
    if (container != nullptr || principal.container.size != 0 ||
        principal.integrity < SECURITY_MANDATORY_MEDIUM_RID || principal.restricted ||
        installation != std::array<uint8_t, 16>{}) {
      return false;
    }
    switch (role) {
      case Role::kLauncher: return uid == 2000;
      case Role::kServiceManager:
      case Role::kSystemServer: return uid == 1000;
      case Role::kApplication: break;
    }
    return false;
  }

  void PruneExitedLocked() {
    for (auto peer = peers_.begin(); peer != peers_.end();) {
      if (WaitForSingleObject(peer->second->process.get(), 0) == WAIT_OBJECT_0) {
        peer = peers_.erase(peer);
      } else {
        ++peer;
      }
    }
  }

  const process_identity::Principal authority_;
  std::mutex mutex_;
  std::map<DWORD, Lease> peers_;
  bool active_ = true;
};

}  // namespace gesher::binder_peer
