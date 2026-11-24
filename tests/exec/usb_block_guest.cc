// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <gesher/usb_block.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>

#include "../../compat/usb_storage/block_broker.h"
#include "../../compat/usb_storage/block_channel.h"

namespace {
using namespace gesher::usb_storage;

// Test-only broker peer. The fixture checks the real guest POSIX path and
// protocol; it never opens or represents a physical Windows disk.
bool Transfer(const BlockChannel& channel, void* bytes, DWORD size, bool write) {
  OVERLAPPED io{};
  io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!io.hEvent) return false;
  DWORD n = 0;
  BOOL ok = write ? WriteFile(channel.server(), bytes, size, &n, &io)
                  : ReadFile(channel.server(), bytes, size, &n, &io);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    HANDLE wait[] = {channel.revocation(), io.hEvent};
    const DWORD result = WaitForMultipleObjects(2, wait, FALSE, 5000);
    if (result != WAIT_OBJECT_0 + 1) {
      CancelIoEx(channel.server(), &io);
      GetOverlappedResult(channel.server(), &io, &n, TRUE);
    } else {
      ok = GetOverlappedResult(channel.server(), &io, &n, FALSE);
    }
  }
  CloseHandle(io.hEvent);
  return ok && n == size;
}

bool Send(const BlockChannel& channel, void* bytes, DWORD size) {
  BlockReply reply{0, size};
  return Transfer(channel, &reply, sizeof(reply), true) &&
         (size == 0 || Transfer(channel, bytes, size, true));
}

bool Serve(const BlockChannel& channel) {
  BlockRequest request{};
  if (!Transfer(channel, &request, sizeof(request), false) ||
      request.operation != BlockOperation::kDescribe) return false;
  BlockDescription description{8192, 4096, kBlockWritable | kBlockRemovable, 3, 0};
  std::array<std::byte, sizeof(description) + 3> bytes{};
  std::memcpy(bytes.data(), &description, sizeof(description));
  const auto model = std::span{bytes}.last<3>();
  std::memcpy(model.data(), "USB", model.size());
  if (!Send(channel, bytes.data(), bytes.size())) return false;

  // BLKROGET observes the authoritative current medium state, not only the
  // attachment snapshot (a software read-only flag can change in a helper).
  if (!Transfer(channel, &request, sizeof(request), false) ||
      request.operation != BlockOperation::kDescribe ||
      !Send(channel, bytes.data(), bytes.size())) return false;

  char payload[] = {'r', 'a', 'w'};
  if (!Transfer(channel, &request, sizeof(request), false) ||
      request.operation != BlockOperation::kRead || request.offset != 513 || request.length != 3 ||
      !Send(channel, payload, sizeof(payload))) return false;
  if (!Transfer(channel, &request, sizeof(request), false) ||
      request.operation != BlockOperation::kWrite || request.offset != 516 || request.length != 2 ||
      !Transfer(channel, payload, 2, false) || std::memcmp(payload, "io", 2) != 0 ||
      !Send(channel, nullptr, 0)) return false;
  return true;
}
}  // namespace

int main() {
  struct stat info{};
  if (stat("/dev/block/sdb", &info) == 0) return 1;
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return 2;
  const HANDLE self = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, GetCurrentProcessId());
  if (!self) return 2;
  BlockGuestHandles handles;
  const DWORD handoff = channel->Handoff(self, &handles);
  CloseHandle(self);
  if (handoff != ERROR_SUCCESS) return 2;
  bool served = false;
  std::thread server([&] {
    served = Serve(*channel);
    if (!served) channel->Revoke();
  });
  int result = 0;
  auto test = [&]() {
    if (gesher::usb_block::AttachBroker(handles.pipe, handles.gate, handles.revoked) != 0) return 3;
    if (stat("/dev/block/sdb", &info) != 0 || !S_ISBLK(info.st_mode)) return 4;
    DIR* entries = opendir("/sys/block");
    if (entries == nullptr) return 5;
    bool found = false;
    while (auto* entry = readdir(entries)) {
      found |= std::strcmp(entry->d_name, "sdb") == 0;
    }
    closedir(entries);
    if (!found) return 6;
    const int fields = open("/sys/block/sdb/size", O_RDONLY);
    char data[16]{};
    if (fields < 0 || read(fields, data, sizeof(data)) != 3 ||
        std::memcmp(data, "16\n", 3) != 0) return 7;
    close(fields);
    const int read_only_fd = open("/dev/block/sdb", O_RDONLY);
    int device_read_only = -1;
    if (read_only_fd < 0 ||
        ioctl(read_only_fd, BLKROGET, &device_read_only) != 0 ||
        device_read_only != 0) return 14;
    close(read_only_fd);
    const int disk = open("/dev/block/sdb", O_RDWR);
    if (disk < 0 || fstat(disk, &info) != 0 || !S_ISBLK(info.st_mode)) return 8;
    uint64_t bytes = 0;
    int sector = 0;
    if (ioctl(disk, 0x80081272, &bytes) != 0 || bytes != 8192 ||
        ioctl(disk, 0x1268, &sector) != 0 || sector != 4096) return 13;
    if (lseek(disk, 513, SEEK_SET) != 513 || read(disk, data, 3) != 3 ||
        std::memcmp(data, "raw", 3) != 0) return 9;
    const int alias = dup(disk);
    if (alias < 0 || lseek(alias, 0, SEEK_CUR) != 516 ||
        write(alias, "io", 2) != 2 || lseek(disk, 0, SEEK_CUR) != 518) return 10;
    close(alias);
    if (channel->Revoke() != ERROR_SUCCESS) return 11;
    if (stat("/dev/block/sdb", &info) == 0 || read(disk, data, 1) >= 0) return 11;
    close(disk);
    return 0;
  };
  result = test();
  if (result != 0) channel->Revoke();
  server.join();
  CloseHandle(handles.revoked);
  CloseHandle(handles.gate);
  CloseHandle(handles.pipe);
  if (result != 0) return result;
  if (!served) return 12;
  std::puts("USB guest block fixture passed");
  return 0;
}
