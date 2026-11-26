// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <gesher/usb_block.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <thread>

#include "../../compat/usb_storage/block_broker_server.h"
#include "../../compat/usb_storage/block_channel.h"
#include "../../compat/usb_storage/host_disk.h"

namespace {

using namespace gesher::usb_storage;

constexpr const char* kGuestDisk = "/dev/block/sdb";
constexpr unsigned long kGetBytes = 0x80081272;
constexpr unsigned long kGetSectorBytes = 0x1268;

int WindowsError(const char* operation, DWORD error) {
  std::fprintf(stderr, "%s failed (Windows error %lu)\n", operation, error);
  return 1;
}

int GuestError(const char* operation) {
  std::fprintf(stderr, "%s failed (guest errno %d)\n", operation, errno);
  return 1;
}

struct GuestFd {
  int value;
  ~GuestFd() { if (value >= 0) close(value); }
};

int VerifyGuestDisk(BlockChannel& channel, const BlockGuestHandles& handles, HostDisk& disk) {
  if (gesher::usb_block::AttachBroker(handles.pipe, handles.gate, handles.revoked) != 0) {
    return GuestError("Attach physical USB disk");
  }
  struct stat info{};
  if (stat(kGuestDisk, &info) != 0) return GuestError("Stat guest disk");
  if (!S_ISBLK(info.st_mode) || (info.st_mode & 0777) != 0400) {
    std::fputs("Expected a read-only guest block device\n", stderr);
    return 1;
  }
  const GuestFd writable{open(kGuestDisk, O_RDWR)};
  if (writable.value >= 0 || errno != EROFS) return GuestError("Reject read-write open");

  const GuestFd fd{open(kGuestDisk, O_RDONLY)};
  if (fd.value < 0) return GuestError("Open guest disk");
  int readonly = -1;
  if (ioctl(fd.value, BLKROGET, &readonly) < 0) {
    return GuestError("Get guest disk read-only state");
  }
  if (readonly != 1) {
    std::fputs("Expected BLKROGET to report a read-only guest disk\n", stderr);
    return 1;
  }
  uint64_t bytes = 0;
  int sector_bytes = 0;
  if (ioctl(fd.value, kGetBytes, &bytes) < 0 ||
      ioctl(fd.value, kGetSectorBytes, &sector_bytes) < 0) {
    return GuestError("Read guest disk geometry");
  }
  const DiskInfo& expected = disk.info();
  if (bytes != expected.bytes || static_cast<DWORD>(sector_bytes) != expected.sector_bytes) {
    std::fputs("Guest geometry differs from physical disk geometry\n", stderr);
    return 1;
  }

  std::vector<std::byte> direct(expected.sector_bytes);
  std::vector<std::byte> guest(expected.sector_bytes);
  for (const uint64_t offset : {uint64_t{0}, bytes - expected.sector_bytes}) {
    const DWORD error = disk.Read(offset, direct);
    if (error != ERROR_SUCCESS) return WindowsError("Read physical USB disk", error);
    if (pread(fd.value, guest.data(), guest.size(), static_cast<off_t>(offset)) !=
        static_cast<ssize_t>(guest.size())) {
      return GuestError("Read guest disk sector");
    }
    if (guest != direct) {
      std::fprintf(stderr, "Guest read differs from physical disk at byte %llu\n",
                   static_cast<unsigned long long>(offset));
      return 1;
    }
  }

  const DWORD revoke = channel.Revoke();
  if (revoke != ERROR_SUCCESS) return WindowsError("Revoke disk session", revoke);
  if (stat(kGuestDisk, &info) == 0 || errno != ENOENT) return GuestError("Hide revoked guest disk");
  if (pread(fd.value, guest.data(), guest.size(), 0) >= 0 || errno != ENODEV) {
    return GuestError("Reject reads on retained descriptor after revocation");
  }
  return 0;
}

}  // namespace

// Manual validation only. Pass the interface path printed by
// usb_storage_probe --list, with a physical USB disk attached and Windows
// access to open it for raw reads. No guest application or runtime is elevated.
int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::fputs("Usage: gesher_usb_block_physical_test <USB disk interface path>\n", stderr);
    return 2;
  }
  std::unique_ptr<HostDisk> disk;
  DWORD error = HostDisk::Open(argv[1], Access::kReadOnly, &disk);
  if (error != ERROR_SUCCESS) return WindowsError("Open physical USB disk read-only", error);

  std::unique_ptr<BlockChannel> channel;
  error = BlockChannel::Create(&channel);
  if (error != ERROR_SUCCESS) return WindowsError("Create private block channel", error);
  const HANDLE self = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, GetCurrentProcessId());
  if (!self) return WindowsError("Open fixture process", GetLastError());
  BlockGuestHandles handles;
  error = channel->Handoff(self, &handles);
  CloseHandle(self);
  if (error != ERROR_SUCCESS) return WindowsError("Transfer guest handles", error);
  DWORD server_status = ERROR_SUCCESS;
  std::thread server([&] {
    server_status = ServeBlockRequests(*channel, *disk);
  });
  const int result = VerifyGuestDisk(*channel, handles, *disk);
  channel->Revoke();
  server.join();
  CloseHandle(handles.revoked);
  CloseHandle(handles.gate);
  CloseHandle(handles.pipe);
  if (result != 0) return result;
  if (server_status != ERROR_OPERATION_ABORTED) {
    return WindowsError("Stop physical USB disk broker", server_status);
  }
  std::puts("Physical USB disk geometry, sector reads, and guest revocation passed");
  return 0;
}
