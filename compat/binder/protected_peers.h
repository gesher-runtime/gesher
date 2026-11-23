// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace gesher::binder {

class ProtectedPeers;

class ProtectedPeer final {
 public:
  ~ProtectedPeer();
  ProtectedPeer(const ProtectedPeer&) = delete;
  ProtectedPeer& operator=(const ProtectedPeer&) = delete;

  // Borrowed, with PROCESS_DUP_HANDLE, SYNCHRONIZE and query-limited access.
  // Keep the peer reference while using it. A revoked peer still owns the
  // original object for unwinding a transfer already in progress.
  HANDLE process() const { return process_; }
  DWORD pid() const { return pid_; }
  DWORD session() const { return session_; }
  uint64_t creation_time() const { return creation_time_; }
  uint32_t uid() const { return uid_; }
  bool live() const;

 private:
  friend class ProtectedPeers;
  ProtectedPeer() = default;

  HANDLE process_ = nullptr;
  DWORD pid_ = 0;
  DWORD session_ = 0;
  uint64_t creation_time_ = 0;
  uint32_t uid_ = 0;
  std::atomic<bool> revoked_ = false;
};

// Private authority-side Binder registration state. A supported launcher must
// hand over the actual process handle from CreateProcess before resuming the
// guest, and authenticate the private transport before calling Register. The
// supplied handle needs READ_CONTROL and must remain valid throughout the call.
// This class is not reachable over the public Binder wire protocol.
class ProtectedPeers final {
 public:
  using Peer = std::shared_ptr<const ProtectedPeer>;

  DWORD Register(HANDLE launched_process, uint32_t android_uid, Peer* result);

  // Use only with an authority-protected duplex pipe server while the connection is
  // stable. The Windows kernel supplies PID and session. The retained process
  // object, not an OpenProcess lookup, supplies the epoch and transfer rights.
  DWORD FindClient(HANDLE server_pipe, Peer* result) const;

  // A revoked live process cannot be rebound to another UID. Scavenge its
  // record after it exits. Existing peer refs can still clean up its handles.
  void Revoke(const Peer& peer);

#ifdef GESHER_BINDER_PEER_FIXTURE
  // This translation unit is built separately for an unprivileged native
  // fixture. The Windows process owner is the fixture's real Windows account.
  DWORD RegisterForFixture(HANDLE launched_process, uint32_t android_uid,
                           PSID fixture_owner, Peer* result);
#endif

 private:
  DWORD RegisterOwned(HANDLE launched_process, uint32_t android_uid, PSID owner, Peer* result);

  mutable std::mutex mutex_;
  std::map<DWORD, std::shared_ptr<ProtectedPeer>> peers_;
};

}  // namespace gesher::binder
