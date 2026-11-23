// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

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

// The trusted launcher supplies retained handles for an authorized endpoint,
// its shared mutex and the broker's manual-reset shutdown event. The caller
// owns the handles and keeps them live. Every process sharing the endpoint
// must use the same mutex and shutdown event throughout the session.
class BlockClient {
 public:
  BlockClient(HANDLE pipe, HANDLE gate, HANDLE shutdown)
      : pipe_(pipe), gate_(gate), shutdown_(shutdown) {}
  DWORD Describe(ClientDiskInfo* info);
  DWORD Read(uint64_t offset, std::span<std::byte> output);
  DWORD Write(uint64_t offset, std::span<const std::byte> input);

 private:
  DWORD Exchange(BlockRequest request, std::span<const std::byte> input,
                 std::span<std::byte> output, DWORD* size);
  HANDLE pipe_;
  HANDLE gate_;
  HANDLE shutdown_;
};

}  // namespace gesher::usb_storage
