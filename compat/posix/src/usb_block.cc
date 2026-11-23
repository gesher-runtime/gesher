// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "usb_block_internal.h"

#include <gesher/usb_block.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <limits>

#include "../../usb_storage/block_broker_client.h"
#include "ofd_internal.h"

namespace gesher::usb_block {
namespace {

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;

Handle Duplicate(HANDLE handle) {
  HANDLE copy = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                       &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return {};
  return Handle(copy);
}

struct Session {
  Handle mutex;
  Handle shutdown;
  usb_storage::BlockClient client;
  usb_storage::ClientDiskInfo info;

  Session(Handle pipe_handle, Handle mutex_handle, Handle shutdown_handle)
      : mutex(std::move(mutex_handle)), shutdown(std::move(shutdown_handle)),
        client(pipe_handle.release(), mutex.get(), shutdown.get()) {}

  bool Live() const { return client.Live(); }
  bool Writable() const { return (info.disk.flags & usb_storage::kBlockWritable) != 0; }
};

std::mutex session_mutex;
std::shared_ptr<Session> session;

std::shared_ptr<Session> Current() {
  std::lock_guard lock(session_mutex);
  return session && session->Live() ? session : nullptr;
}

int BlockError(DWORD error) {
  if (error == ERROR_MEDIA_CHANGED || error == ERROR_DEVICE_NOT_CONNECTED ||
      error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED ||
      error == ERROR_NO_MEDIA_IN_DRIVE) return errno = ENODEV, -1;
  if (error == ERROR_WRITE_PROTECT) return errno = EROFS, -1;
  return errno = ErrnoFromWin32(error), -1;
}

void FillStat(const Session& active, struct stat* info) {
  memset(info, 0, sizeof(*info));
  info->st_mode = S_IFBLK | (active.Writable() ? 0600 : 0400);
  info->st_ino = 1;
  info->st_nlink = 1;
  info->st_blksize = active.info.disk.sector_bytes;
  // Major 8, minor 16 is the Linux block device number for sdb.
  info->st_rdev = (8 << 8) | 16;
}

}  // namespace

struct Description {
  std::shared_ptr<Session> session;
  std::mutex mutex;
  uint64_t position = 0;
};

int AttachBroker(HANDLE pipe, HANDLE mutex, HANDLE shutdown) {
  auto retained_pipe = Duplicate(pipe);
  if (!retained_pipe) return FailWithLastError();
  auto retained_mutex = Duplicate(mutex);
  if (!retained_mutex) return FailWithLastError();
  auto retained_shutdown = Duplicate(shutdown);
  if (!retained_shutdown) return FailWithLastError();
  if (GetFileType(retained_pipe.get()) != FILE_TYPE_PIPE) return errno = EINVAL, -1;
  auto active = std::make_shared<Session>(std::move(retained_pipe), std::move(retained_mutex),
                                          std::move(retained_shutdown));
  const DWORD error = active->client.Describe(&active->info);
  if (error != ERROR_SUCCESS) return BlockError(error);
  if (active->info.disk.bytes > std::numeric_limits<int64_t>::max()) {
    return errno = EOPNOTSUPP, -1;
  }
  std::lock_guard lock(session_mutex);
  if (session) return errno = EBUSY, -1;
  session = std::move(active);
  return 0;
}

bool Attached() { return Current() != nullptr; }
bool Writable() {
  auto active = Current();
  return active && active->Writable();
}

bool SysContent(const std::string& name, std::string* content) {
  auto active = Current();
  if (!active) return false;
  if (name == "size") *content = std::to_string(active->info.disk.bytes / 512);
  else if (name == "removable") *content = (active->info.disk.flags & usb_storage::kBlockRemovable) ? "1" : "0";
  else if (name == "ro") *content = active->Writable() ? "0" : "1";
  else if (name == "model" && !active->info.model.empty()) *content = active->info.model;
  else if (name == "serial" && !active->info.serial.empty()) *content = active->info.serial;
  else return false;
  content->push_back('\n');
  return true;
}

int Open(int flags) {
  auto active = Current();
  if (!active) return errno = ENOENT, -1;
  if ((flags & O_DIRECTORY) != 0) return errno = ENOTDIR, -1;
  if ((flags & (O_PATH | O_APPEND | O_TRUNC | O_DIRECT | O_ASYNC)) != 0) {
    return errno = EOPNOTSUPP, -1;
  }
  if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return errno = EEXIST, -1;
  if ((flags & O_ACCMODE) != O_RDONLY && !active->Writable()) return errno = EROFS, -1;
  if ((flags & O_ACCMODE) == O_ACCMODE) return errno = EINVAL, -1;

  const int fd = ReserveFd();
  if (fd < 0) return -1;
  if (RegisterOfd(fd, flags | O_CLOEXEC) < 0 ||
      SetDescriptorKind(fd, DescriptorKind::kUnshareable) < 0) {
    const int saved = errno;
    close(fd);
    return errno = saved, -1;
  }
  auto snapshot = CaptureDescriptor(fd);
  if (!snapshot) {
    const int saved = errno;
    close(fd);
    return errno = saved, -1;
  }
  auto description = std::make_shared<Description>();
  description->session = std::move(active);
  snapshot->description->usb_block = std::move(description);
  return fd;
}

int Stat(struct stat* info) {
  auto active = Current();
  if (!active) return errno = ENOENT, -1;
  FillStat(*active, info);
  return 0;
}

bool StatFd(const DescriptorSnapshotPtr& snapshot, struct stat* info, int* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  FillStat(*block->session, info);
  *result = 0;
  return true;
}

bool Io(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,
        bool writing, int64_t requested_offset, int* result) {
  if (!snapshot || !snapshot->description) return false;
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  auto io = [&]() -> int {
    if (!block->session->Live()) return errno = ENODEV, -1;
    const int access = snapshot->description->Flags() & O_ACCMODE;
    if ((writing && access == O_RDONLY) || (!writing && access == O_WRONLY)) {
      return errno = EBADF, -1;
    }
    if (size == 0) return 0;
    if (!data) return errno = EFAULT, -1;
    std::lock_guard lock(block->mutex);
    const uint64_t position = requested_offset < 0 ? block->position : requested_offset;
    const uint64_t bytes = block->session->info.disk.bytes;
    if (position >= bytes) return writing ? (errno = ENOSPC, -1) : 0;
    const auto count = std::min<uint64_t>({size, usb_storage::kBlockTransferLimit, bytes - position});
    auto payload = std::span(static_cast<std::byte*>(data), static_cast<size_t>(count));
    const DWORD error = writing ? block->session->client.Write(position, payload)
                                : block->session->client.Read(position, payload);
    if (error != ERROR_SUCCESS) return BlockError(error);
    if (requested_offset < 0) block->position += count;
    return static_cast<int>(count);
  };
  *result = io();
  return true;
}

bool Seek(const DescriptorSnapshotPtr& snapshot, int64_t offset, int whence, int64_t* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  std::lock_guard lock(block->mutex);
  const int64_t length = static_cast<int64_t>(block->session->info.disk.bytes);
  int64_t base = 0;
  switch (whence) {
    case SEEK_SET: break;
    case SEEK_CUR: base = static_cast<int64_t>(block->position); break;
    case SEEK_END: base = length; break;
    default: errno = EINVAL; *result = -1; return true;
  }
  if (offset < -base || offset > length - base) {
    errno = EINVAL;
    *result = -1;
    return true;
  }
  *result = base + offset;
  block->position = static_cast<uint64_t>(*result);
  return true;
}

bool Sync(const DescriptorSnapshotPtr& snapshot, int* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  // The broker flushes every successful write before returning the result.
  *result = block->session->Live() ? 0 : (errno = ENODEV, -1);
  return true;
}

bool Ioctl(const DescriptorSnapshotPtr& snapshot, unsigned long operation,
           void* output, int* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  // Standard Linux UAPI commands on a block device. Configuration controls
  // deliberately remain unavailable to the restricted guest.
  constexpr unsigned long kBlockGetReadOnly = 0x125e;
  constexpr unsigned long kBlockGetSectorBytes = 0x1268;
  constexpr unsigned long kBlockGetByteSize = 0x80081272;
  auto dispatch = [&]() -> int {
    if (operation != kBlockGetReadOnly && operation != kBlockGetSectorBytes &&
        operation != kBlockGetByteSize) return errno = ENOTTY, -1;
    if (!block->session->Live()) return errno = ENODEV, -1;
    if (output == nullptr) return errno = EFAULT, -1;
    if (operation == kBlockGetByteSize) {
      const uint64_t bytes = block->session->info.disk.bytes;
      memcpy(output, &bytes, sizeof(bytes));
    } else {
      const int number = operation == kBlockGetReadOnly ? !block->session->Writable()
          : static_cast<int>(block->session->info.disk.sector_bytes);
      memcpy(output, &number, sizeof(number));
    }
    return 0;
  };
  *result = dispatch();
  return true;
}

}  // namespace gesher::usb_block
