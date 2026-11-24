// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <atomic>
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
  // Optional trusted caller status, which must outlive the launch. This also
  // records cleanup of a suspended child if Create fails before enrollment.
  std::atomic<DWORD>* termination_error = nullptr;
  // Optional authority-owned job. Associate it atomically at process creation;
  // no job handle is inherited or supplied through a guest request.
  HANDLE job = nullptr;
};

class AuthorityLaunch {
 public:
  // The unimpersonated LocalSystem service supplies the desktop primary token
  // for its authenticated, selected session. No handles or environment are
  // inherited from the service. The process remains suspended on success.
  static DWORD Create(HANDLE desktop_token, const AuthorityLaunchSpec& spec,
                      std::unique_ptr<AuthorityLaunch>* result,
                      const char** startup_stage = nullptr);
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
  // Idempotent termination with an exact-process wait. Callers may share one
  // deadline across several children; destruction does not repeat that wait.
  DWORD Stop(DWORD timeout_milliseconds);

 private:
  AuthorityLaunch() = default;
  PROCESS_INFORMATION guest_{};
  bool resumed_ = false;
  bool stop_attempted_ = false;
  DWORD stop_status_ = ERROR_SUCCESS;
  std::atomic<DWORD>* termination_error_ = nullptr;
};

}  // namespace gesher::usb_storage
