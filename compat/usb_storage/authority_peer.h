// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <memory>

namespace gesher::usb_storage {

// Authenticates the one AppContainer process created by the trusted launcher.
// The launcher may leave the process suspended until Create succeeds. It must
// supply the process handle from creation, never one looked up from a hello.
class AuthorityPeer {
 public:
  static DWORD Create(HANDLE launched_process, const wchar_t* local_pipe_name,
                      SECURITY_ATTRIBUTES* pipe_security,
                      std::unique_ptr<AuthorityPeer>* result);
  ~AuthorityPeer();
  AuthorityPeer(const AuthorityPeer&) = delete;
  AuthorityPeer& operator=(const AuthorityPeer&) = delete;

  // Borrowed handles. The authority retains both. Never give either to a guest.
  HANDLE pipe() const { return pipe_; }
  HANDLE process() const { return process_; }

  // Call on an unimpersonated worker after reading a complete request. The pipe
  // worker must serialize reads, authentication and any disconnect/reconnect.
  // The client must open its own connection and keep that handle private.
  DWORD Authenticate() const;

 private:
  AuthorityPeer() = default;
  DWORD CheckPipeIdentity() const;

  HANDLE process_ = nullptr;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE token_ = nullptr;
  DWORD pid_ = 0;
  DWORD session_ = 0;
};

}  // namespace gesher::usb_storage
