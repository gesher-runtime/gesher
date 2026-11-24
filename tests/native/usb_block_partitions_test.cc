// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "../../compat/usb_storage/block_partitions.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace gesher::usb_storage;

namespace {

void U32(std::vector<std::byte>& bytes, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = std::byte((value >> (i * 8)) & 255);
}
void U64(std::vector<std::byte>& bytes, size_t at, uint64_t value) {
  U32(bytes, at, static_cast<uint32_t>(value));
  U32(bytes, at + 4, static_cast<uint32_t>(value >> 32));
}
void Signature(std::vector<std::byte>& bytes, size_t at = 0) {
  bytes.at(at + 510) = std::byte{0x55}; bytes.at(at + 511) = std::byte{0xaa};
}
void Entry(std::vector<std::byte>& bytes, size_t at, uint8_t type, uint32_t first, uint32_t count) {
  bytes.at(at + 4) = std::byte(type);
  U32(bytes, at + 8, first); U32(bytes, at + 12, count);
}

PartitionResult Parse(const std::vector<std::byte>& disk, uint32_t sector,
                      std::vector<BlockPartition>* result) {
  return ReadBlockPartitions(disk.size(), sector,
      [&](uint64_t at, std::span<std::byte> bytes) {
        if (at > disk.size() || bytes.size() > disk.size() - at) return false;
        std::memcpy(bytes.data(), disk.data() + at, bytes.size());
        return true;
      }, result);
}

void GptCrcs(std::vector<std::byte>& disk) {
  U32(disk, 512 + 88, partition_detail::Crc32(std::span(disk).subspan(1024, 128 * 128)));
  U32(disk, 512 + 16, 0);
  U32(disk, 512 + 16, partition_detail::Crc32(std::span(disk).subspan(512, 92)));
}

}  // namespace

int main() {
  std::vector<BlockPartition> result;
  std::vector<std::byte> disk(1024 * 1024);
  assert(Parse(disk, 512, &result) == PartitionResult::kOk && result.empty());
  Signature(disk);
  Entry(disk, 446, 0x0c, 64, 100);
  Entry(disk, 478, 0x83, 256, 512);
  assert(Parse(disk, 512, &result) == PartitionResult::kOk && result.size() == 2);
  assert(result[0].number == 1 && result[0].offset == 64 * 512 && result[0].bytes == 100 * 512);
  assert(result[1].number == 3 && result[1].offset == 256 * 512);
  Entry(disk, 478, 0x83, 100, 512);
  assert(Parse(disk, 512, &result) == PartitionResult::kInvalidTable && result.empty());
  Entry(disk, 478, 0x83, UINT32_MAX, UINT32_MAX);
  assert(Parse(disk, 512, &result) == PartitionResult::kInvalidTable);
  Entry(disk, 478, 0, 0, 0);
  assert(Parse(disk, 4096, &result) == PartitionResult::kOk && result[0].offset == 64 * 4096);

  // EBR links are relative to the extended container, not the previous data
  // partition. Cycles and extents outside that container must be refused.
  disk.assign(disk.size(), std::byte{0});
  Signature(disk); Entry(disk, 446, 0x0f, 64, 1024);
  Signature(disk, 64 * 512); Entry(disk, 64 * 512 + 446, 0x0c, 1, 63);
  Entry(disk, 64 * 512 + 462, 0x0f, 128, 896);
  Signature(disk, 192 * 512); Entry(disk, 192 * 512 + 446, 0x83, 1, 127);
  assert(Parse(disk, 512, &result) == PartitionResult::kOk && result.size() == 2);
  assert(result[0].number == 5 && result[1].number == 6 && result[1].offset == 193 * 512);
  Entry(disk, 192 * 512 + 462, 0x0f, 128, 896);
  assert(Parse(disk, 512, &result) == PartitionResult::kInvalidTable);

  disk.assign(disk.size(), std::byte{0});
  Signature(disk); Entry(disk, 446, 0xee, 1, 2047);
  std::memcpy(disk.data() + 512, "EFI PART", 8);
  U32(disk, 512 + 8, 0x10000); U32(disk, 512 + 12, 92);
  U64(disk, 512 + 24, 1); U64(disk, 512 + 32, 2047);
  U64(disk, 512 + 40, 34); U64(disk, 512 + 48, 2014);
  U64(disk, 512 + 72, 2); U32(disk, 512 + 80, 128); U32(disk, 512 + 84, 128);
  disk[1024] = std::byte{1}; U64(disk, 1024 + 32, 64); U64(disk, 1024 + 40, 255);
  GptCrcs(disk);
  assert(Parse(disk, 512, &result) == PartitionResult::kOk && result.size() == 1);
  assert(result[0].bytes == 192 * 512);
  disk[512 + 56] ^= std::byte{1};
  assert(Parse(disk, 512, &result) == PartitionResult::kInvalidTable);
  GptCrcs(disk); disk[1024 + 127] ^= std::byte{1};
  assert(Parse(disk, 512, &result) == PartitionResult::kInvalidTable);
  GptCrcs(disk); U64(disk, 1024 + 40, UINT64_MAX); GptCrcs(disk);
  assert(Parse(disk, 512, &result) == PartitionResult::kInvalidTable);
  assert(ReadBlockPartitions(4096, 512, [](uint64_t, std::span<std::byte>) { return false; },
                             &result) == PartitionResult::kIoError);
  std::puts("USB on-media partition contracts passed");
}
