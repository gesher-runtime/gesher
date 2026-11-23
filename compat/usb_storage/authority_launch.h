// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <memory>
#include <span>
#include <string_view>

namespace gesher::usb_storage {

struct AuthorityLaunchSpec {
  // Paths, arguments, complete Unicode environment and capabilities are supplied
  // by the trusted authority from its validated installation and session.
  const wchar_t* executable = nullptr;
  const wchar_t* directory = nullptr;
  std::wstring_view arguments;
  std::span<const wchar_t> environment;
  SECURITY_CAPABILITIES capabilities{};
  PSID desktop_user = nullptr;
  DWORD desktop_session = 0;
};

class AuthorityLaunch {
 public:
  // The unimpersonated LocalSystem service supplies the desktop primary token
  // for its authenticated, selected session. No handles or environment are
  // inherited from the service. The process remains suspended on success.
  static DWORD Create(HANDLE desktop_token, const AuthorityLaunchSpec& spec,
                      std::unique_ptr<AuthorityLaunch>* result);
  ~AuthorityLaunch();
  AuthorityLaunch(const AuthorityLaunch&) = delete;
  AuthorityLaunch& operator=(const AuthorityLaunch&) = delete;

  // Borrowed authority-side process handle. Never grant it to a guest.
  HANDLE process() const { return guest_.hProcess; }
  DWORD pid() const { return guest_.dwProcessId; }
  DWORD main_thread_id() const { return guest_.dwThreadId; }

  // The authority serializes launch, handle handoff and resume. It retains the
  // launch object for the lifetime of the session. Destruction stops the guest.
  DWORD Resume();

 private:
  AuthorityLaunch() = default;
  PROCESS_INFORMATION guest_{};
  bool resumed_ = false;
};

}  // namespace gesher::usb_storage
