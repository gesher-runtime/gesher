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
  bool write_consented = false;
  std::string model;
  std::string serial;
};

struct HostDiskIdentity {
  uint64_t bytes = 0;
  uint32_t sector_bytes = 0;
  uint32_t media_generation = 0;
  // removable1, writable2, generation4, ejection-lock8, volume-lock-mode16
  uint32_t flags = 0;
  uint32_t volume_count = 0;
};
static_assert(sizeof(HostDiskIdentity) == 24);

// Disk-interface links currently confirmed to represent USB storage. Descriptor
// queries that fail omit the candidate. Paths are for trusted host selection.
DWORD ListDisks(std::vector<std::wstring>* paths);

// One authorized Windows disk and media session. The owner must make the
// selection and authorize access before handing the object to a guest broker.
class HostDisk {
 public:
  static DWORD Open(const std::wstring& path, Access access,
                    std::unique_ptr<HostDisk>* disk);
  // Upgrade a chooser selection through its retained device handle. Call
  // only after the user approves write access to this particular medium.
  DWORD OpenSelectedForWrite(std::unique_ptr<HostDisk>* disk);
  // The elevated, installed chooser keeps the selected object live while
  // obtaining consent. The authority duplicates this exact object from that
  // authenticated chooser, never reopens a path after the dialog.
  DWORD DuplicateForAuthority(HANDLE* result, HostDiskIdentity* identity,
                              std::vector<HANDLE>* volume_handles = nullptr);
  // Consumes owned_handle on every outcome. Expected identity came from the
  // trusted chooser and is checked against the still-open exact object.
  static DWORD Adopt(HANDLE owned_handle, Access access, const HostDiskIdentity& expected,
                     std::unique_ptr<HostDisk>* disk, std::vector<HANDLE> volume_handles = {});
  // Call in the chooser only after the authority acknowledges adoption. Its
  // copy now owns the lock; closing the chooser must not unlock the same FILE_OBJECT.
  void ReleaseAfterAuthorityHandoff();
  ~HostDisk();
  HostDisk(const HostDisk&) = delete;
  HostDisk& operator=(const HostDisk&) = delete;

  const DiskInfo& info() const { return info_; }
  DWORD Validate();
  DWORD Describe(DiskInfo* info);
  DWORD SetReadOnly(bool readonly);
  DWORD Flush();
  DWORD Read(uint64_t offset, std::span<std::byte> output);
  DWORD Write(uint64_t offset, std::span<const std::byte> input);

 private:
  explicit HostDisk(HANDLE handle) : handle_(handle) {}
  DWORD Inspect();
  DWORD PrepareWriteAccess();
  DWORD CheckMedia();
  DWORD VerifyWriteAccess();
  DWORD VerifyExclusion();
  DWORD LockVolumes();
  bool ValidRange(uint64_t offset, size_t size) const;

  HANDLE handle_;
  DiskInfo info_;
  std::mutex mutex_;
  ULONG media_generation_ = 0;
  bool track_media_ = false;
  bool prevent_removal_ = false;
  bool volume_lock_mode_ = false;
  DWORD disk_number_ = MAXDWORD;
  std::vector<HANDLE> volume_handles_;
  std::vector<std::wstring> volume_names_;
  bool disconnected_ = false;
};

}  // namespace gesher::usb_storage
