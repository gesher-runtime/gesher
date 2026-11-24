// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::usb_storage::runtime_wire {
inline constexpr uint32_t kMagic = 0x47525054;
inline constexpr uint32_t kVersion = 2;
inline constexpr uint32_t kMaximumPayload = 64 * 1024;
enum class Operation : uint32_t {
  kSpawn = 1, kStatus, kKill, kGrantBlock, kRevokeBlock, kInitialize, kSpawnCommand, kSignal
};
struct Request {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  Operation operation{};
  uint32_t payload_bytes = 0;
  uint32_t pid = 0;
  uint32_t uid = 0;
  uint32_t target_sdk = 0;
  uint32_t flags = 0;
  uint64_t start_time = 0;
  uint64_t start_sequence = 0;
  uint32_t lifetime_milliseconds = 0;
  uint32_t reserved = 0;
  uint64_t standard_handles[3]{};
};
struct Reply {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  uint32_t status = 0;
  uint32_t pid = 0;
  uint64_t start_time = 0;
  uint32_t exit_code = 0;
  uint32_t running = 0;
  uint32_t reserved = 0;
  uint32_t reserved2 = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint64_t standard_handles[3]{};
  uint64_t block_pipe = 0;
  uint64_t block_gate = 0;
  uint64_t block_revoked = 0;
  uint64_t wait_handle = 0;
  uint32_t role = 0;
  uint32_t reserved3 = 0;
  uint64_t signal_section = 0;
  uint64_t signal_event = 0;
  uint64_t log_mutex = 0;
};
static_assert(sizeof(Request) == 80 && sizeof(Reply) == 136);

inline bool Append(std::vector<std::byte>* data, std::u16string_view text) {
  if (text.size() > 8192 || text.find(u'\0') != std::u16string_view::npos) return false;
  const auto size = static_cast<uint32_t>(text.size());
  const size_t bytes = sizeof(size) + text.size() * sizeof(char16_t);
  if (data->size() > kMaximumPayload - bytes) return false;
  const size_t previous = data->size();
  data->resize(previous + bytes);
  std::memcpy(data->data() + previous, &size, sizeof(size));
  std::memcpy(data->data() + previous + sizeof(size), text.data(), text.size() * sizeof(char16_t));
  return true;
}

class Reader {
 public:
  explicit Reader(const std::vector<std::byte>& data) : data_(data) {}
  bool Read(std::u16string* text) {
    if (offset_ > data_.size() || data_.size() - offset_ < sizeof(uint32_t)) return false;
    uint32_t count = 0;
    std::memcpy(&count, data_.data() + offset_, sizeof(count));
    offset_ += sizeof(count);
    if (count > 8192 || count > (data_.size() - offset_) / sizeof(char16_t)) return false;
    text->resize(count);
    std::memcpy(text->data(), data_.data() + offset_, count * sizeof(char16_t));
    offset_ += count * sizeof(char16_t);
    return text->find(u'\0') == std::u16string::npos;
  }
  bool empty() const { return offset_ == data_.size(); }
 private:
  const std::vector<std::byte>& data_;
  size_t offset_ = 0;
};
}  // namespace gesher::usb_storage::runtime_wire
