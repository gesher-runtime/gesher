// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace gesher::rooted_storage_wire {

constexpr uint32_t kMagic = 0x47535253;  // GSRS
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxPathUnits = 32767;

enum class Op : uint32_t {
  kOpen = 1, kMakeDirectory, kRemove, kRename, kStatAndroidData, kOpenBuildProp, kOpenFrameworkRes,
};
constexpr bool OpensHandle(Op op) {
  return op == Op::kOpen || op == Op::kOpenBuildProp || op == Op::kOpenFrameworkRes;
}
constexpr uint32_t kCredential = 0;
constexpr uint32_t kDevice = 1;
constexpr uint32_t kFirstExternalArea = 2;
constexpr uint32_t kExternalAreaCount = 3;
constexpr uint32_t kAreaCount = kFirstExternalArea + kExternalAreaCount;

// For kOpen the low bits name the requested access. The remaining bits are
// booleans. kRemove accepts only kDirectory, and kRename only kReplace.
constexpr uint32_t kRead = 0;
constexpr uint32_t kWrite = 1;
constexpr uint32_t kReadWrite = 2;
constexpr uint32_t kMetadata = 3;
constexpr uint32_t kMetadataWrite = 4;
constexpr uint32_t kAccessMask = 0x7;
constexpr uint32_t kCreate = 1u << 3;
constexpr uint32_t kExclusive = 1u << 4;
constexpr uint32_t kTruncate = 1u << 5;
constexpr uint32_t kDirectory = 1u << 6;
constexpr uint32_t kReadOnlyCreate = 1u << 7;
constexpr uint32_t kWriteThrough = 1u << 8;
constexpr uint32_t kReplace = 1u << 9;
constexpr uint32_t kShareOverride = 1u << 10;
constexpr uint32_t kShareShift = 11;
constexpr uint32_t kShareMask = 0x7u << kShareShift;
constexpr uint32_t kDeleteOnClose = 1u << 14;
constexpr uint32_t kOverlapped = 1u << 15;
constexpr uint32_t kOpenFlags = kAccessMask | kCreate | kExclusive | kTruncate |
                                kDirectory | kReadOnlyCreate | kWriteThrough |
                                kShareOverride | kShareMask | kDeleteOnClose | kOverlapped;

// A request is followed by path_units then target_units little-endian UTF-16
// code units without terminators. An empty path identifies its selected root.
struct Request {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  Op operation{};
  uint32_t source_area = kCredential;
  uint32_t target_area = kCredential;
  uint32_t flags = 0;
  uint32_t path_units = 0;
  uint32_t target_units = 0;
};

constexpr uint32_t kCreated = 1;
struct Reply {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  int32_t status = 0;  // Positive Linux errno, or 0 on success.
  uint32_t flags = 0;
  uint64_t handle = 0;  // Only a successful kOpen supplies a guest-owned handle.
};

// Only kStatAndroidData can return this trailing frame. It has no input path
// or flags and reports the actual filesystem backing this run's Android /data.
struct Filesystem {
  uint64_t available_bytes = 0;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
};

// A successful kOpen is an offer. The guest echoes OpenAck with kOpenAckMagic
// and owns the handle only after the host replies with kOpenCommitMagic.
constexpr uint32_t kOpenAckMagic = 0x47535241;     // GSRA
constexpr uint32_t kOpenCommitMagic = 0x47535243;  // GSRC
struct OpenAck {
  uint32_t magic = kOpenAckMagic;
  uint32_t version = kVersion;
};

static_assert(sizeof(Request) == 32 && sizeof(Reply) == 24 && sizeof(Filesystem) == 24 && sizeof(OpenAck) == 8);

}  // namespace gesher::rooted_storage_wire
