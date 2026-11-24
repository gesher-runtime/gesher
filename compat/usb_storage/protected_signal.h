// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

namespace gesher::usb_storage {

// The only bytes exchanged by the dedicated signal channel. Both objects are
// unnamed and authority-created; an app-supplied mapping is never imported.
struct ProtectedSignalMailbox {
  volatile LONG64 pending;
  volatile LONG senders[65];
};

class ProtectedSignalChannel {
 public:
  static DWORD Create(HANDLE retained_child, std::unique_ptr<ProtectedSignalChannel>* result);
  ~ProtectedSignalChannel();
  ProtectedSignalChannel(const ProtectedSignalChannel&) = delete;
  ProtectedSignalChannel& operator=(const ProtectedSignalChannel&) = delete;
  DWORD Handoff(HANDLE* child_section, HANDLE* child_event);
  // The caller has already checked the exact parent/child relationship. The
  // retained process object is used even if the caller's numeric PID is reused.
  DWORD Send(int signal, DWORD sender_pid = 0);

 private:
  ProtectedSignalChannel() = default;
  HANDLE child_ = nullptr;
  HANDLE section_ = nullptr;
  HANDLE event_ = nullptr;
  ProtectedSignalMailbox* view_ = nullptr;
  bool handed_off_ = false;
};

}  // namespace gesher::usb_storage
