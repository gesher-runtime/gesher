// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

namespace gesher::usb_storage {

// The broker owns the disk for the whole connection. A client receives an
// already-authorized private pipe endpoint, never a raw Windows disk handle.
// Each request is followed by its payload (write only) and a single reply.
// Integers are little-endian on the supported Windows architectures.
constexpr uint32_t kBlockProtocolVersion = 1;
constexpr uint32_t kBlockTransferLimit = 1024 * 1024;

enum class BlockOperation : uint32_t {
  kDescribe = 1,
  kRead = 2,
  kWrite = 3,
  kFlush = 4,
  kSetReadOnly = 5,
};

struct BlockRequest {
  uint32_t version;
  BlockOperation operation;
  uint32_t length;
  uint32_t reserved;
  uint64_t offset;
};
static_assert(sizeof(BlockRequest) == 24);

struct BlockReply {
  uint32_t error;  // Windows error, zero on success.
  uint32_t length;
};
static_assert(sizeof(BlockReply) == 8);

// Descriptions include model and serial as two adjacent UTF-8 strings after
// this fixed-size record, without NUL terminators.
constexpr uint32_t kBlockWritable = 1;
constexpr uint32_t kBlockRemovable = 2;
constexpr uint32_t kBlockWriteConsent = 4;
constexpr uint32_t kMaxBlockDescriptionString = 1024;
struct BlockDescription {
  uint64_t bytes;
  uint32_t sector_bytes;
  uint32_t flags;
  uint32_t model_bytes;
  uint32_t serial_bytes;
};
static_assert(sizeof(BlockDescription) == 24);

}  // namespace gesher::usb_storage
