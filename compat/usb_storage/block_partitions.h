// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gesher::usb_storage {

struct BlockPartition {
  uint32_t number;
  uint64_t offset;
  uint64_t bytes;
};

enum class PartitionResult { kOk, kIoError, kInvalidTable, kUnsupported };

namespace partition_detail {

inline uint32_t U32(std::span<const std::byte> bytes, size_t at) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<uint32_t>(bytes[at + i]) << (8 * i);
  return value;
}
inline uint64_t U64(std::span<const std::byte> bytes, size_t at) {
  return U32(bytes, at) | (static_cast<uint64_t>(U32(bytes, at + 4)) << 32);
}
inline uint32_t Crc32(std::span<const std::byte> bytes) {
  uint32_t value = UINT32_MAX;
  for (std::byte byte : bytes) {
    value ^= std::to_integer<uint8_t>(byte);
    for (unsigned bit = 0; bit < 8; ++bit) {
      value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
  }
  return ~value;
}
inline bool Extended(uint8_t type) { return type == 5 || type == 15 || type == 0x85; }
inline bool Signature(std::span<const std::byte> sector) {
  return sector.size() >= 512 && sector[510] == std::byte{0x55} && sector[511] == std::byte{0xaa};
}
inline bool Add(std::vector<BlockPartition>* output, uint32_t number,
                uint64_t first, uint64_t sectors, uint64_t total, uint32_t sector_bytes) {
  if (!first || !sectors || first >= total || sectors > total - first) return false;
  const BlockPartition item{number, first * sector_bytes, sectors * sector_bytes};
  for (const auto& existing : *output) {
    if (item.offset < existing.offset + existing.bytes &&
        existing.offset < item.offset + item.bytes) return false;
  }
  output->push_back(item);
  return true;
}

}  // namespace partition_detail

// Read on-media partition metadata through the same bounded authorized disk
// transport as data. The callback must read the complete requested span. No
// partition is synthesized from filesystem labels, capacity or a host path.
// A corrupt table fails closed; a disk without a partition table has no children.
template <typename Read>
PartitionResult ReadBlockPartitions(uint64_t bytes, uint32_t sector_bytes,
                                    Read read, std::vector<BlockPartition>* output) {
  using namespace partition_detail;
  if (!output || sector_bytes < 512 || sector_bytes > 65536 ||
      (sector_bytes & (sector_bytes - 1)) || bytes == 0 || bytes % sector_bytes) {
    return PartitionResult::kInvalidTable;
  }
  output->clear();
  const uint64_t total = bytes / sector_bytes;
  std::vector<BlockPartition> found;
  std::array<std::byte, 512> mbr{};
  if (!read(0, std::span(mbr))) return PartitionResult::kIoError;
  if (!Signature(mbr)) return PartitionResult::kOk;
  bool gpt = false;
  for (size_t entry = 446; entry < 510; entry += 16) {
    gpt |= mbr[entry + 4] == std::byte{0xee};
  }
  if (gpt) {
    if (total < 3) return PartitionResult::kInvalidTable;
    std::vector<std::byte> header(sector_bytes);
    if (!read(sector_bytes, std::span(header))) return PartitionResult::kIoError;
    constexpr std::array signature{std::byte{'E'}, std::byte{'F'}, std::byte{'I'}, std::byte{' '},
                                   std::byte{'P'}, std::byte{'A'}, std::byte{'R'}, std::byte{'T'}};
    if (!std::equal(signature.begin(), signature.end(), header.begin())) return PartitionResult::kInvalidTable;
    const uint32_t header_size = U32(header, 12);
    const uint32_t header_crc = U32(header, 16);
    if (U32(header, 8) != 0x10000 || header_size < 92 || header_size > sector_bytes ||
        U32(header, 20) != 0 || U64(header, 24) != 1 || U64(header, 32) >= total ||
        U64(header, 32) <= 1) return PartitionResult::kInvalidTable;
    std::fill(header.begin() + 16, header.begin() + 20, std::byte{0});
    if (Crc32(std::span(header).first(header_size)) != header_crc) return PartitionResult::kInvalidTable;
    const uint64_t first_usable = U64(header, 40);
    const uint64_t last_usable = U64(header, 48);
    const uint64_t table_lba = U64(header, 72);
    const uint32_t count = U32(header, 80), entry_bytes = U32(header, 84);
    const uint32_t table_crc = U32(header, 88);
    if (count == 0 || entry_bytes < 128 || entry_bytes % 8) return PartitionResult::kInvalidTable;
    if (count > 128 || entry_bytes > 4096) return PartitionResult::kUnsupported;
    const uint64_t table_bytes = static_cast<uint64_t>(count) * entry_bytes;
    const uint64_t table_sectors = (table_bytes + sector_bytes - 1) / sector_bytes;
    if (first_usable < 2 || first_usable > last_usable || last_usable >= total - 1 ||
        table_lba < 2 || table_lba >= first_usable || table_sectors > first_usable - table_lba) {
      return PartitionResult::kInvalidTable;
    }
    std::vector<std::byte> table(static_cast<size_t>(table_bytes));
    if (!read(table_lba * sector_bytes, std::span(table))) return PartitionResult::kIoError;
    if (Crc32(table) != table_crc) return PartitionResult::kInvalidTable;
    for (uint32_t i = 0; i < count; ++i) {
      const auto entry = std::span(table).subspan(static_cast<size_t>(i) * entry_bytes, entry_bytes);
      if (std::all_of(entry.begin(), entry.begin() + 16, [](std::byte b) { return b == std::byte{0}; })) continue;
      const uint64_t first = U64(entry, 32), last = U64(entry, 40);
      if (first < first_usable || last > last_usable || first > last ||
          !Add(&found, i + 1, first, last - first + 1, total, sector_bytes)) {
        return PartitionResult::kInvalidTable;
      }
    }
  } else {
    uint64_t extended_first = 0, extended_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      const size_t at = 446 + i * 16;
      const auto type = std::to_integer<uint8_t>(mbr[at + 4]);
      const uint64_t first = U32(mbr, at + 8), count = U32(mbr, at + 12);
      if (!type) {
        if (first || count) return PartitionResult::kInvalidTable;
        continue;
      }
      if (Extended(type)) {
        if (extended_first || !first || !count || first >= total || count > total - first) {
          return PartitionResult::kInvalidTable;
        }
        extended_first = first;
        extended_count = count;
      } else if (!Add(&found, i + 1, first, count, total, sector_bytes)) {
        return PartitionResult::kInvalidTable;
      }
    }
    if (extended_first) {
      const uint64_t extended_end = extended_first + extended_count;
      uint64_t current = extended_first;
      std::vector<uint64_t> visited;
      for (uint32_t number = 5; ; ++number) {
        if (visited.size() >= 124) return PartitionResult::kUnsupported;
        if (current < extended_first || current >= extended_end ||
            std::find(visited.begin(), visited.end(), current) != visited.end()) {
          return PartitionResult::kInvalidTable;
        }
        visited.push_back(current);
        std::array<std::byte, 512> ebr{};
        if (!read(current * sector_bytes, std::span(ebr))) return PartitionResult::kIoError;
        if (!Signature(ebr)) return PartitionResult::kInvalidTable;
        const auto type = std::to_integer<uint8_t>(ebr[450]);
        const uint64_t relative = U32(ebr, 454), count = U32(ebr, 458);
        if (!type || Extended(type) || !relative || relative >= extended_end - current ||
            count > extended_end - current - relative ||
            !Add(&found, number, current + relative, count, total, sector_bytes)) {
          return PartitionResult::kInvalidTable;
        }
        const auto next_type = std::to_integer<uint8_t>(ebr[466]);
        const uint64_t next = U32(ebr, 470), next_count = U32(ebr, 474);
        if (!next_type && !next && !next_count) break;
        if (!Extended(next_type) || !next || !next_count || next >= extended_count ||
            next_count > extended_count - next) return PartitionResult::kInvalidTable;
        current = extended_first + next;
      }
    }
  }
  *output = std::move(found);
  return PartitionResult::kOk;
}

}  // namespace gesher::usb_storage
