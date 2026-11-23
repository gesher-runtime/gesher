// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "../binder/protected_bootstrap_wire.h"

namespace gesher::usb_storage {

class AuthorityLaunch;
class AuthorityPeer;
class BinderListenerFactory;

// Private capability transfer from the Windows service to a broker the same
// service has created with AuthorityLaunch. The session's owner serializes
// calls and keeps its shutdown event live until this control is destroyed.
class BinderAuthorityControl {
 public:
  static DWORD CreateLocator(std::wstring* locator);
  // Publish both private endpoints and grant the broker query access before
  // resuming it. Keep query access until both channels authenticate.
  static DWORD Connect(AuthorityLaunch& broker, const std::wstring& registration_locator,
                       const std::wstring& listener_locator, HANDLE stop,
                       std::unique_ptr<BinderAuthorityControl>* registration,
                       std::unique_ptr<BinderAuthorityControl>* listeners);
  ~BinderAuthorityControl();
  BinderAuthorityControl(const BinderAuthorityControl&) = delete;
  BinderAuthorityControl& operator=(const BinderAuthorityControl&) = delete;

  DWORD Register(uint64_t id, uint32_t uid, binder::PeerRole role, const AuthorityLaunch& process);
  DWORD Revoke(uint64_t id);
  // Dedicated worker. Connection failure retires the endpoint and its broker.
  DWORD ServeListeners(BinderListenerFactory& factory);

 private:
  BinderAuthorityControl() = default;
  static DWORD Prepare(AuthorityLaunch& broker, const std::wstring& locator, HANDLE stop,
                       binder::bootstrap_wire::Role role, SECURITY_ATTRIBUTES& security,
                       std::unique_ptr<BinderAuthorityControl>* result);
  DWORD Handshake();
  DWORD Transfer(void* data, DWORD bytes, bool writing, bool wait_idle = false);
  DWORD Exchange(binder::bootstrap_wire::Operation op, uint64_t id,
                 uint32_t uid, binder::PeerRole role, HANDLE remote_handle);
  DWORD FailClosed(DWORD status);

  // This retained handle can retire the exact broker on an ambiguous exchange.
  // Neither the handle nor the server endpoint leaves the authority.
  HANDLE broker_ = nullptr;
  DWORD broker_pid_ = 0;
  HANDLE stop_ = nullptr;
  binder::bootstrap_wire::Role role_ = binder::bootstrap_wire::Role::kRegistration;
  std::unique_ptr<AuthorityPeer> peer_;
};

}  // namespace gesher::usb_storage
