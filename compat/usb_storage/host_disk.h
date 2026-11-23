// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace gesher::usb_storage {

constexpr DWORD kMaxTransfer = 1024 * 1024;

enum class Access { kReadOnly, kOfflineReadWrite };

struct DiskInfo {
  uint64_t bytes = 0;
  DWORD sector_bytes = 0;
  bool removable = false;
  bool writable = false;
  std::string model;
  std::string serial;
};

// Disk-interface links currently confirmed to represent USB storage. Descriptor
// queries that fail omit the candidate. Paths are for trusted host selection.
DWORD ListDisks(std::vector<std::wstring>* paths);

// One authorized Windows disk and media session. The owner must make the
// selection and authorize access before handing the object to a guest broker.
class HostDisk {
 public:
  static DWORD Open(const std::wstring& path, Access access,
                    std::unique_ptr<HostDisk>* disk);
  ~HostDisk();
  HostDisk(const HostDisk&) = delete;
  HostDisk& operator=(const HostDisk&) = delete;

  const DiskInfo& info() const { return info_; }
  DWORD Validate();
  DWORD Read(uint64_t offset, std::span<std::byte> output);
  DWORD Write(uint64_t offset, std::span<const std::byte> input);

 private:
  explicit HostDisk(HANDLE handle) : handle_(handle) {}
  DWORD CheckMedia();
  bool ValidRange(uint64_t offset, size_t size) const;

  HANDLE handle_;
  DiskInfo info_;
  std::mutex mutex_;
  ULONG media_generation_ = 0;
  bool track_media_ = false;
  bool prevent_removal_ = false;
  bool disconnected_ = false;
};

}  // namespace gesher::usb_storage
