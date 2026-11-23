// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "block_broker.h"

namespace gesher::usb_storage {

struct ClientDiskInfo {
  BlockDescription disk{};
  std::string model;
  std::string serial;
};

// One client owns the stream state in the process selected by the launcher.
// The client takes ownership of the private data endpoint. The caller retains
// the mutex and wait-only host revocation handle.
class BlockClient {
 public:
  BlockClient(HANDLE pipe, HANDLE gate, HANDLE shutdown)
      : pipe_(pipe), gate_(gate), shutdown_(shutdown) {}
  ~BlockClient();
  DWORD Describe(ClientDiskInfo* info);
  DWORD Read(uint64_t offset, std::span<std::byte> output);
  DWORD Write(uint64_t offset, std::span<const std::byte> input);
  bool Live() const;

 private:
  DWORD Exchange(BlockRequest request, std::span<const std::byte> input,
                 std::span<std::byte> output, BlockDescription* description);
  DWORD Poison(DWORD error);
  HANDLE pipe_;
  HANDLE gate_;
  HANDLE shutdown_;
  std::atomic<bool> poisoned_{false};
};

}  // namespace gesher::usb_storage
