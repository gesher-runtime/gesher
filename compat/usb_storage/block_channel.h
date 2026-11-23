// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <memory>

namespace gesher::usb_storage {

// The authority establishes this channel locally before it admits a guest.
// No handle is inheritable. The trusted launcher must transfer only client,
// gate and shutdown to the process it authorized, never server.
struct BlockChannel {
  ~BlockChannel();
  BlockChannel(const BlockChannel&) = delete;
  BlockChannel& operator=(const BlockChannel&) = delete;

  static DWORD Create(std::unique_ptr<BlockChannel>* result);
  HANDLE server = INVALID_HANDLE_VALUE;
  HANDLE client = INVALID_HANDLE_VALUE;
  HANDLE gate = nullptr;
  HANDLE shutdown = nullptr;

 private:
  BlockChannel() = default;
};

}  // namespace gesher::usb_storage
