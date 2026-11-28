// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

struct AHardwareBuffer;

namespace gesher::bionic::ahb {

// The fixed description accompanying a shared backing descriptor in Gesher.
// The version 1 layout is also used by the NDK Unix socket transfer.
struct BufferTransfer {
  uint32_t magic;
  uint32_t version;
  uint32_t width, height, layers, format;
  uint64_t usage;
  uint32_t stride, reserved;
  uint64_t id;
  uint64_t size;
};
static_assert(sizeof(BufferTransfer) == 56);

// Synchronizes available pixels and supplies a borrowed fd. The caller
// must keep the source reference alive until the transfer duplicates it.
int PrepareBufferTransfer(const AHardwareBuffer* buffer, BufferTransfer* transfer, int* fd);

// Consumes fd on every path. On success the caller owns one buffer reference.
int ImportBufferTransfer(const BufferTransfer& transfer, int fd, AHardwareBuffer** out);

}  // namespace gesher::bionic::ahb
