// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <memory>
#include <mutex>

namespace gesher::usb_storage {

struct BlockGuestHandles {
  HANDLE pipe = nullptr;
  HANDLE gate = nullptr;
  HANDLE revoked = nullptr;
};

// The authority owns the server and the power to terminate the session.
class BlockChannel {
 public:
  ~BlockChannel();
  BlockChannel(const BlockChannel&) = delete;
  BlockChannel& operator=(const BlockChannel&) = delete;

  static DWORD Create(std::unique_ptr<BlockChannel>* result);

  // Borrowed authority-side handles. Never transfer them to the guest.
  HANDLE server() const { return server_; }
  HANDLE revocation() const { return revocation_; }
  DWORD Revoke();

  // Duplicate exactly once into the process object created and authorized by
  // the trusted launcher. It needs PROCESS_DUP_HANDLE and SYNCHRONIZE. Keep
  // the process suspended or in a trusted bootstrap until handoff is complete.
  // Values are valid only in that process and are not inheritable. No values
  // are published on failure and provisional copies are closed.
  DWORD Handoff(HANDLE process, BlockGuestHandles* result);

 private:
  BlockChannel() = default;
  HANDLE server_ = INVALID_HANDLE_VALUE;
  HANDLE client_ = INVALID_HANDLE_VALUE;
  HANDLE gate_ = nullptr;
  HANDLE revocation_ = nullptr;
  std::mutex handoff_mutex_;
};

}  // namespace gesher::usb_storage
