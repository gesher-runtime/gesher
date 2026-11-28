// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protected_peers.h"
#include "protected_bootstrap_wire.h"

#include <memory>
#include <string>
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

  // Use a separate bootstrap connection and a fixed private data-pipe name.
  static DWORD ConnectListeners(std::wstring_view locator, std::wstring_view data_locator,
                                std::unique_ptr<ProtectedBootstrap>* result);

  // Call serially with an empty output. Success gives the caller one owning
  // listener handle. A transport, authority, or validation error permanently
  // closes this connection. The protected broker must exit on such an error.
  DWORD TakeListener(HANDLE* listener);

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
  static DWORD AdoptFixtureListenerPipe(HANDLE connected_pipe, std::wstring_view data_locator,
                                        std::unique_ptr<ProtectedBootstrap>* result);
#endif

 private:
  ProtectedBootstrap() = default;
  static DWORD ConnectRole(std::wstring_view locator, bootstrap_wire::Role role,
                           std::wstring_view data_locator,
                           std::unique_ptr<ProtectedBootstrap>* result);
#ifdef GESHER_BINDER_BOOTSTRAP_FIXTURE
  static DWORD AdoptFixtureRole(HANDLE connected_pipe, bootstrap_wire::Role role,
                                std::wstring_view data_locator,
                                std::unique_ptr<ProtectedBootstrap>* result);
#endif
  DWORD CheckLiveAuthority() const;
  DWORD CheckListener(HANDLE listener) const;
  DWORD RequestListener(HANDLE* listener);
  void ClosePipe();
  DWORD Handshake();
  DWORD Transfer(void* data, DWORD size, bool sending, DWORD timeout);
  DWORD Reply(uint64_t registration_id, DWORD status);

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  SC_HANDLE service_ = nullptr;
  DWORD server_pid_ = 0;
  DWORD server_session_ = 0;
  bootstrap_wire::Role role_ = bootstrap_wire::Role::kRegistration;
  std::wstring data_locator_;
};

}  // namespace gesher::binder
