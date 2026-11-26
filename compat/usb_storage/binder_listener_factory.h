// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::usb_storage {

class AuthorityLaunch;

// One protected data endpoint owned by the USB authority for one binderd
// process. Only the authority can create additional named-pipe instances.
class BinderListenerFactory final {
 public:
  static DWORD CreateLocator(std::wstring* locator);
  static DWORD Create(const AuthorityLaunch& broker, std::wstring_view locator,
                      std::span<PSID const> app_containers,
                      std::unique_ptr<BinderListenerFactory>* result);
  ~BinderListenerFactory();
  BinderListenerFactory(const BinderListenerFactory&) = delete;
  BinderListenerFactory& operator=(const BinderListenerFactory&) = delete;

  const std::wstring& name() const { return name_; }
  DWORD broker_pid() const { return GetProcessId(broker_); }

  // Returns a noninheritable value in the exact broker process's handle table.
  // The authority publishes it only over its authenticated private control
  // connection. If publication is ambiguous, revoke the session and do not
  // try to close this value remotely: the broker might already have reused it.
  DWORD Take(HANDLE* broker_listener);

  // The authority adds an exact enrolled package SID before registering and
  // resuming its app. All instances share the guard's protected pipe DACL.
  // This grants connection access only; binderd still admits exact retained
  // process objects and their Android identity independently.
  DWORD AllowAppContainer(PSID package);

  // Stop vending listeners, and stop the exact process to retire all listeners
  // already offered. This is the lifetime of Binder, independent of a USB grant.
  DWORD Revoke();

#ifdef GESHER_BINDER_LISTENER_FACTORY_FIXTURE
  // Separately linked native fixture. The production library has no alternate
  // owner, process source or SCM identity.
  static DWORD CreateForFixture(HANDLE broker, PSID owner, std::span<PSID const> app_containers,
                                std::unique_ptr<BinderListenerFactory>* result);
#endif

 private:
  BinderListenerFactory() = default;
  static DWORD CreateOwned(HANDLE broker, PSID owner, std::wstring_view locator,
                           std::span<PSID const> app_containers,
                           std::unique_ptr<BinderListenerFactory>* result);

  std::wstring name_;
  HANDLE broker_ = nullptr;
  HANDLE guard_ = INVALID_HANDLE_VALUE;
  HANDLE guard_client_ = INVALID_HANDLE_VALUE;
  std::vector<BYTE> owner_sid_;
  std::vector<std::vector<BYTE>> package_sids_;
  std::mutex mutex_;
  bool published_ = false;
  bool revoked_ = false;
};

}  // namespace gesher::usb_storage
