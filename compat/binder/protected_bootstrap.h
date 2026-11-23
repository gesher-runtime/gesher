// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protected_peers.h"

#include <memory>
#include <string_view>

namespace gesher::binder {

// Private receiving endpoint for the protected launcher. This is independent
// of ordinary Binder discovery and cannot be opened via the guest protocol.
class ProtectedBootstrap final {
 public:
  ~ProtectedBootstrap();
  ProtectedBootstrap(const ProtectedBootstrap&) = delete;
  ProtectedBootstrap& operator=(const ProtectedBootstrap&) = delete;

  // Authenticate the real running SCM authority on this connected pipe,
  // then complete a bounded Hello/Ready handshake. The locator carries no
  // authentication and may be supplied on a command line.
  static DWORD Connect(std::wstring_view locator,
                       std::unique_ptr<ProtectedBootstrap>* result);

  // Consume registration frames on the same connection, once. On any fatal
  // error or EOF, revoke every registration made over this connection before
  // returning. The protected broker must exit and must never fall back to
  // the ordinary named-pipe broker or PID-open transfer on return.
  DWORD Serve(ProtectedPeers& peers);

#ifdef GESHER_BINDER_BOOTSTRAP_FIXTURE
  // Separately linked transport fixture; no alternate authority identity is
  // available in a production consumer.
  static DWORD AdoptFixturePipe(HANDLE connected_pipe,
                                std::unique_ptr<ProtectedBootstrap>* result);
#endif

 private:
  ProtectedBootstrap() = default;
  DWORD CheckLiveAuthority() const;
  DWORD Handshake();
  DWORD Transfer(void* data, DWORD size, bool sending, DWORD timeout);
  DWORD Reply(uint64_t registration_id, DWORD status);

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  SC_HANDLE service_ = nullptr;
  DWORD server_pid_ = 0;
};

}  // namespace gesher::binder
