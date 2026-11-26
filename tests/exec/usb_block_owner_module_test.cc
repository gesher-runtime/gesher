// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// This DLL links another complete gesher_posix copy. Calls must still execute
// against the executable's admitted USB session and descriptor ownership.
#include <gesher/usb_block.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

extern "C" __declspec(dllexport)
int UsbOwnerAttach(HANDLE pipe, HANDLE gate, HANDLE revoked) {
  return gesher::usb_block::AttachBroker(pipe, gate, revoked);
}

extern "C" __declspec(dllexport) int UsbOwnerQuery() {
  struct stat info{};
  if (stat("/dev/block/sdb", &info) != 0) return errno == ENOENT || errno == ENODEV ? 0 : -1;
  if (!S_ISBLK(info.st_mode)) return -2;
  const int fd = open("/sys/block/sdb/size", O_RDONLY);
  if (fd < 0) return -3;
  char size[8]{};
  const auto count = read(fd, size, sizeof(size));
  close(fd);
  if (count != 3 || std::memcmp(size, "16\n", 3) != 0) return -4;
  const int directory = open("/sys/dev/block/8:17", O_RDONLY | O_DIRECTORY);
  if (directory < 0 || fstat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) return -5;
  const int number = openat(directory, "partition", O_RDONLY);
  char value[16]{};
  if (number < 0 || read(number, value, sizeof(value)) != 2 || std::memcmp(value, "1\n", 2)) return -6;
  close(number);
  const int parent_size = openat(directory, "../size", O_RDONLY);
  if (parent_size < 0 || read(parent_size, value, sizeof(value)) != 3 || std::memcmp(value, "16\n", 3)) return -7;
  close(parent_size);
  const int offset = openat(directory, "start", O_RDONLY);
  if (offset < 0 || read(offset, value, sizeof(value)) != 2 || std::memcmp(value, "1\n", 2)) return -8;
  close(offset);
  DIR* entries = fdopendir(dup(directory));
  if (!entries) return -9;
  bool partition_seen = false;
  while (const dirent* item = readdir(entries)) partition_seen |= std::strcmp(item->d_name, "partition") == 0;
  closedir(entries);
  const bool no_file_read = read(directory, value, 1) == -1 && errno == EISDIR;
  close(directory);
  if (!partition_seen || !no_file_read) return -10;
  if (lstat("/sys/dev/block/8:16", &info) || !S_ISLNK(info.st_mode)) return -11;
  if (lstat("/sys/block/sdb/device", &info) || !S_ISLNK(info.st_mode)) return -12;
  return 1;
}

extern "C" __declspec(dllexport) int UsbOwnerOpen() {
  return open("/dev/block/sdb", O_RDWR);
}

extern "C" __declspec(dllexport)
int UsbOwnerRead(int fd, char* bytes, unsigned count, int64_t offset) {
  return static_cast<int>(pread(fd, bytes, count, offset));
}

extern "C" __declspec(dllexport)
int UsbOwnerWrite(int fd, const char* bytes, unsigned count, int64_t offset) {
  return static_cast<int>(pwrite(fd, bytes, count, offset));
}

extern "C" __declspec(dllexport) int UsbOwnerClose(int fd) { return close(fd); }

extern "C" __declspec(dllexport) int UsbOwnerRevokedRead(int fd) {
  char value = 0;
  return pread(fd, &value, 1, 0) == -1 && errno == ENODEV ? 1 : 0;
}
