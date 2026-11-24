// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gesher/file_description.h>
#include <sys/stat.h>
#include <string>
#include <string_view>
#include <vector>
#include <charconv>

#include "internal.h"

namespace gesher::usb_block {

inline constexpr const char* kBlockName = "sdb";
inline std::string DeviceNumberName(std::string_view node) {
  if (node == kBlockName) return "8:16";
  if (!node.starts_with(kBlockName)) return {};
  node.remove_prefix(3);
  uint32_t number = 0;
  const auto parsed = std::from_chars(node.data(), node.data() + node.size(), number);
  if (node.empty() || node.front() == '0' || parsed.ec != std::errc{} ||
      parsed.ptr != node.data() + node.size() || number > 128) return {};
  return number < 16 ? "8:" + std::to_string(16 + number) : "259:" + std::to_string(number - 16);
}
inline std::string NodeFromDeviceNumber(std::string_view value) {
  const size_t colon = value.find(':');
  if (colon == std::string_view::npos) return {};
  uint32_t major = 0, minor = 0;
  const auto a = std::from_chars(value.data(), value.data() + colon, major);
  const auto b = std::from_chars(value.data() + colon + 1, value.data() + value.size(), minor);
  if (a.ec != std::errc{} || b.ec != std::errc{} || a.ptr != value.data() + colon ||
      b.ptr != value.data() + value.size()) return {};
  uint32_t partition = 0;
  if (major == 8 && minor >= 16 && minor < 32) partition = minor - 16;
  else if (major == 259 && minor <= 112) partition = minor + 16;
  else return {};
  const std::string node = partition ? "sdb" + std::to_string(partition) : "sdb";
  return DeviceNumberName(node) == value ? node : std::string{};
}
bool Attached();
bool Writable();
bool SysContent(const std::string& name, std::string* content);
std::vector<std::string> Devices();
bool DeviceExists(std::string_view name);
bool NodeSysContent(std::string_view node, const std::string& name, std::string* content);
int Open(int flags);
int OpenNode(std::string_view node, int flags);
int Stat(struct stat* info);
int StatNode(std::string_view node, struct stat* info);
bool StatFd(const DescriptorSnapshotPtr& snapshot, struct stat* info, int* result);
bool Io(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,
        bool writing, int64_t offset, int* result);
bool Seek(const DescriptorSnapshotPtr& snapshot, int64_t offset, int whence, int64_t* result);
bool Sync(const DescriptorSnapshotPtr& snapshot, int* result);
bool Ioctl(const DescriptorSnapshotPtr& snapshot, unsigned long operation,
           void* output, int* result);

}  // namespace gesher::usb_block
