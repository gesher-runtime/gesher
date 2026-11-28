// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "usb_block_internal.h"

#include <gesher/usb_block.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>

#include "../../usb_storage/block_broker_client.h"
#include "../../usb_storage/block_partitions.h"
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
  std::atomic<bool> writable{false};

  Session(Handle pipe_handle, Handle mutex_handle, Handle shutdown_handle)
      : mutex(std::move(mutex_handle)), shutdown(std::move(shutdown_handle)),
        client(pipe_handle.release(), mutex.get(), shutdown.get()) {}

  bool Live() const { return client.Live(); }
  bool Writable() const { return writable.load(); }
  bool WriteConsented() const {
    return (info.disk.flags & (usb_storage::kBlockWritable | usb_storage::kBlockWriteConsent)) != 0;
  }
  DWORD RefreshReadonly() {
    usb_storage::ClientDiskInfo current;
    const DWORD error = client.Describe(&current);
    if (error) return error;
    if (current.disk.bytes != info.disk.bytes || current.disk.sector_bytes != info.disk.sector_bytes ||
        (current.disk.flags & usb_storage::kBlockRemovable) != (info.disk.flags & usb_storage::kBlockRemovable) ||
        current.model != info.model || current.serial != info.serial) return ERROR_MEDIA_CHANGED;
    writable.store((current.disk.flags & usb_storage::kBlockWritable) != 0);
    return ERROR_SUCCESS;
  }
};

std::mutex session_mutex;
std::shared_ptr<Session> session;

int AttachLocal(HANDLE pipe, HANDLE mutex, HANDLE shutdown);
bool AttachedLocal();
bool WritableLocal();
std::vector<std::string> DevicesLocal();
bool DeviceExistsLocal(std::string_view name);
bool NodeSysContentLocal(std::string_view node, const std::string& name, std::string* content);
int OpenNodeLocal(std::string_view node, int flags);
int StatNodeLocal(std::string_view node, struct stat* info);
bool StatFdLocal(const DescriptorSnapshotPtr& snapshot, struct stat* info, int* result);
bool IoLocal(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,
             bool writing, int64_t requested_offset, int* result);
bool SeekLocal(const DescriptorSnapshotPtr& snapshot, int64_t offset, int whence, int64_t* result);
bool SyncLocal(const DescriptorSnapshotPtr& snapshot, int* result);
bool IoctlLocal(const DescriptorSnapshotPtr& snapshot, unsigned long operation, void* output,
                int* result);

struct OwnerApi {
  uint32_t version;
  uint32_t bytes;
  decltype(&AttachLocal) attach;
  decltype(&AttachedLocal) attached;
  decltype(&WritableLocal) writable;
  decltype(&DevicesLocal) devices;
  decltype(&DeviceExistsLocal) exists;
  decltype(&NodeSysContentLocal) content;
  decltype(&OpenNodeLocal) open;
  decltype(&StatNodeLocal) stat;
  decltype(&StatFdLocal) stat_fd;
  decltype(&IoLocal) io;
  decltype(&SeekLocal) seek;
  decltype(&SyncLocal) sync;
  decltype(&IoctlLocal) ioctl;
};

const OwnerApi* Owner() {
  // gesher_posix is static in both the executable and Android DLLs. The
  // authority attaches its grant during executable startup; every module must
  // use that same stream, revocation state and disk identity. Creation and all
  // operations execute in the owner too: an unloading DLL must not leave a
  // session/description whose destructor points into its retired image. The v1
  // function table is a private same-build ABI, like the process-wide OFD owner.
  using Factory = const OwnerApi* (*)();
  static const auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(nullptr), "gesher_usb_block_owner_v1")));
  const OwnerApi* owner = factory ? factory() : nullptr;
  if (!owner || owner->version != 1 || owner->bytes != sizeof(OwnerApi)) {
    errno = ENOSYS;
    return nullptr;
  }
  return owner;
}

std::shared_ptr<Session> Current() {
  std::lock_guard lock(session_mutex);
  return session && session->Live() ? session : nullptr;
}

int BlockError(DWORD error) {
  if (error == ERROR_MEDIA_CHANGED || error == ERROR_DEVICE_NOT_CONNECTED ||
      error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED ||
      error == ERROR_NO_MEDIA_IN_DRIVE) return errno = ENODEV, -1;
  if (error == ERROR_WRITE_PROTECT) return errno = EROFS, -1;
  if (error == ERROR_READ_FAULT || error == ERROR_WRITE_FAULT || error == ERROR_CRC ||
      error == ERROR_SECTOR_NOT_FOUND || error == ERROR_IO_DEVICE) return errno = EIO, -1;
  return errno = ErrnoFromWin32(error), -1;
}

bool PartitionNumber(std::string_view node, uint32_t* number) {
  if (node == kBlockName) { *number = 0; return true; }
  if (!node.starts_with(kBlockName)) return false;
  node.remove_prefix(strlen(kBlockName));
  if (node.empty() || node.front() == '0') return false;
  const auto parsed = std::from_chars(node.data(), node.data() + node.size(), *number);
  return parsed.ec == std::errc{} && parsed.ptr == node.data() + node.size() &&
      *number != 0 && *number <= 128;
}

bool Partitions(Session& active, std::vector<usb_storage::BlockPartition>* result) {
  DWORD io_error = ERROR_SUCCESS;
  const auto status = usb_storage::ReadBlockPartitions(active.info.disk.bytes,
      active.info.disk.sector_bytes, [&](uint64_t offset, std::span<std::byte> bytes) {
        io_error = active.client.Read(offset, bytes);
        return io_error == ERROR_SUCCESS;
      }, result);
  if (status == usb_storage::PartitionResult::kOk) return true;
  if (status == usb_storage::PartitionResult::kIoError) BlockError(io_error);
  else errno = status == usb_storage::PartitionResult::kUnsupported ? EOPNOTSUPP : EIO;
  return false;
}

bool Extent(Session& active, std::string_view node, usb_storage::BlockPartition* result) {
  uint32_t number = 0;
  if (!PartitionNumber(node, &number)) return errno = ENOENT, false;
  if (!number) {
    *result = {0, 0, active.info.disk.bytes};
    return true;
  }
  std::vector<usb_storage::BlockPartition> entries;
  if (!Partitions(active, &entries)) return false;
  const auto found = std::find_if(entries.begin(), entries.end(),
      [number](const auto& entry) { return entry.number == number; });
  if (found == entries.end()) return errno = ENOENT, false;
  *result = *found;
  return true;
}

void FillStat(const Session& active, const usb_storage::BlockPartition& extent, struct stat* info) {
  memset(info, 0, sizeof(*info));
  info->st_mode = S_IFBLK | (active.Writable() ? 0600 : 0400);
  info->st_ino = 1 + extent.number;
  info->st_nlink = 1;
  info->st_blksize = active.info.disk.sector_bytes;
  // Major 8, minor 16 is the Linux block device number for sdb.
  info->st_rdev = extent.number < 16 ? (8 << 8) | (16 + extent.number)
                                    : (259 << 8) | (extent.number - 16);
}

}  // namespace

struct Description {
  std::shared_ptr<Session> session;
  usb_storage::BlockPartition extent{};
  std::mutex mutex;
  uint64_t position = 0;
};

namespace {

int AttachLocal(HANDLE pipe, HANDLE mutex, HANDLE shutdown) {
  {
    std::lock_guard lock(session_mutex);
    // A second module cannot replace the selected device or consume another
    // protocol frame on the active stream just by attaching the same handles.
    if (session) return errno = EBUSY, -1;
  }
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
  active->writable.store((active->info.disk.flags & usb_storage::kBlockWritable) != 0);
  if (active->info.disk.bytes > std::numeric_limits<int64_t>::max()) {
    return errno = EOPNOTSUPP, -1;
  }
  std::lock_guard lock(session_mutex);
  if (session) return errno = EBUSY, -1;
  session = std::move(active);
  return 0;
}

bool AttachedLocal() { return Current() != nullptr; }
bool WritableLocal() {
  auto active = Current();
  return active && active->RefreshReadonly() == ERROR_SUCCESS && active->Writable();
}

std::vector<std::string> DevicesLocal() {
  auto active = Current();
  if (!active) return {};
  std::vector<std::string> nodes{kBlockName};
  std::vector<usb_storage::BlockPartition> entries;
  if (Partitions(*active, &entries)) {
    for (const auto& entry : entries) nodes.push_back(std::string(kBlockName) + std::to_string(entry.number));
  }
  return nodes;
}

bool DeviceExistsLocal(std::string_view name) {
  auto active = Current();
  usb_storage::BlockPartition extent{};
  return active && Extent(*active, name, &extent);
}

bool NodeSysContentLocal(std::string_view node, const std::string& name, std::string* content) {
  auto active = Current();
  usb_storage::BlockPartition extent{};
  if (!active || !Extent(*active, node, &extent)) return false;
  if (name == "size") *content = std::to_string(extent.bytes / 512);
  else if (name == "start" && extent.number) *content = std::to_string(extent.offset / 512);
  else if (name == "partition" && extent.number) *content = std::to_string(extent.number);
  else if (name == "removable") *content = (active->info.disk.flags & usb_storage::kBlockRemovable) ? "1" : "0";
  else if (name == "ro") {
    const DWORD error = active->RefreshReadonly();
    if (error) { BlockError(error); return false; }
    *content = active->Writable() ? "0" : "1";
  }
  else if (name == "model" && !active->info.model.empty()) *content = active->info.model;
  else if (name == "serial" && !active->info.serial.empty()) *content = active->info.serial;
  else return false;
  content->push_back('\n');
  return true;
}

int OpenNodeLocal(std::string_view node, int flags) {
  auto active = Current();
  if (!active) return errno = ENOENT, -1;
  usb_storage::BlockPartition extent{};
  if (!Extent(*active, node, &extent)) return -1;
  if ((flags & O_DIRECTORY) != 0) return errno = ENOTDIR, -1;
  if ((flags & (O_PATH | O_APPEND | O_TRUNC | O_DIRECT | O_ASYNC)) != 0) {
    return errno = EOPNOTSUPP, -1;
  }
  if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return errno = EEXIST, -1;
  if ((flags & O_ACCMODE) != O_RDONLY && !active->Writable()) {
    if (!active->WriteConsented()) return errno = EROFS, -1;
    const DWORD error = active->RefreshReadonly();
    if (error) return BlockError(error);
    if (!active->Writable()) return errno = EROFS, -1;
  }
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
  description->extent = extent;
  snapshot->description->usb_block = std::move(description);
  return fd;
}

int StatNodeLocal(std::string_view node, struct stat* info) {
  auto active = Current();
  if (!active) return errno = ENOENT, -1;
  usb_storage::BlockPartition extent{};
  if (!Extent(*active, node, &extent)) return -1;
  FillStat(*active, extent, info);
  return 0;
}

bool StatFdLocal(const DescriptorSnapshotPtr& snapshot, struct stat* info, int* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  FillStat(*block->session, block->extent, info);
  *result = 0;
  return true;
}

bool IoLocal(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,
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
    const uint64_t bytes = block->extent.bytes;
    if (position >= bytes) return writing ? (errno = ENOSPC, -1) : 0;
    const auto count = std::min<uint64_t>({size,
        static_cast<uint64_t>(std::numeric_limits<int>::max()), bytes - position});
    if (position > UINT64_MAX - block->extent.offset ||
        count > UINT64_MAX - block->extent.offset - position) return errno = EOVERFLOW, -1;
    const uint64_t disk_position = block->extent.offset + position;
    uint64_t completed = 0;
    while (completed < count) {
      if (!block->session->Live()) {
        errno = ENODEV;
        return completed ? static_cast<int>(completed) : -1;
      }
      // The wire limit bounds one transaction, not a successful POSIX call.
      // Each exchange revalidates the live grant and actual media at the
      // authority. Stop at the first failure; count only acknowledged chunks.
      const auto chunk = std::min<uint64_t>(count - completed, usb_storage::kBlockTransferLimit);
      auto payload = std::span(static_cast<std::byte*>(data) + completed, static_cast<size_t>(chunk));
      const DWORD error = writing ? block->session->client.Write(disk_position + completed, payload)
                                  : block->session->client.Read(disk_position + completed, payload);
      if (error != ERROR_SUCCESS) {
        BlockError(error);
        return completed ? static_cast<int>(completed) : -1;
      }
      completed += chunk;
      if (requested_offset < 0) block->position += chunk;
    }
    return static_cast<int>(completed);
  };
  *result = io();
  return true;
}

bool SeekLocal(const DescriptorSnapshotPtr& snapshot, int64_t offset, int whence, int64_t* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  std::lock_guard lock(block->mutex);
  const int64_t length = static_cast<int64_t>(block->extent.bytes);
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

bool SyncLocal(const DescriptorSnapshotPtr& snapshot, int* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  const DWORD error = block->session->client.Flush();
  *result = error ? BlockError(error) : 0;
  return true;
}

bool IoctlLocal(const DescriptorSnapshotPtr& snapshot, unsigned long operation,
           void* output, int* result) {
  auto block = snapshot->description->usb_block;
  if (!block) return false;
  // Only the selected block object is affected. Read-only changes require the
  // authority's still-live explicit write consent and a real host operation.
  constexpr unsigned long kBlockSetReadOnly = 0x125d;
  constexpr unsigned long kBlockGetReadOnly = 0x125e;
  constexpr unsigned long kBlockGetSectors = 0x1260;
  constexpr unsigned long kBlockFlush = 0x1261;
  constexpr unsigned long kBlockGetSectorBytes = 0x1268;
  constexpr unsigned long kBlockGetByteSize = 0x80081272;
  auto dispatch = [&]() -> int {
    if (operation != kBlockGetReadOnly && operation != kBlockSetReadOnly && operation != kBlockFlush &&
        operation != kBlockGetSectors && operation != kBlockGetSectorBytes &&
        operation != kBlockGetByteSize) return errno = ENOTTY, -1;
    if (!block->session->Live()) return errno = ENODEV, -1;
    if (operation == kBlockFlush) {
      const DWORD error = block->session->client.Flush();
      return error ? BlockError(error) : 0;
    }
    if (output == nullptr) return errno = EFAULT, -1;
    if (operation == kBlockSetReadOnly) {
      if (!block->session->WriteConsented()) return errno = EACCES, -1;
      int readonly = 0;
      memcpy(&readonly, output, sizeof(readonly));
      if (readonly != 0 && readonly != 1) return errno = EINVAL, -1;
      DWORD error = block->session->client.SetReadOnly(readonly != 0);
      if (!error) error = block->session->RefreshReadonly();
      return error ? BlockError(error) : 0;
    }
    if (operation == kBlockGetReadOnly) {
      const DWORD error = block->session->RefreshReadonly();
      if (error) return BlockError(error);
    }
    if (operation == kBlockGetByteSize || operation == kBlockGetSectors) {
      const uint64_t bytes = operation == kBlockGetByteSize ? block->extent.bytes : block->extent.bytes / 512;
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

}  // namespace

extern "C" void gesher_usb_block_owner_anchor_v1() {}
extern "C" __declspec(dllexport) const OwnerApi* gesher_usb_block_owner_v1() {
  static const OwnerApi api{1, sizeof(OwnerApi), AttachLocal, AttachedLocal, WritableLocal,
      DevicesLocal, DeviceExistsLocal, NodeSysContentLocal, OpenNodeLocal, StatNodeLocal,
      StatFdLocal, IoLocal, SeekLocal, SyncLocal, IoctlLocal};
  return &api;
}

int AttachBroker(HANDLE pipe, HANDLE mutex, HANDLE shutdown) {
  const auto* owner = Owner();
  return owner ? owner->attach(pipe, mutex, shutdown) : -1;
}
bool Attached() {
  const auto* owner = Owner();
  return owner && owner->attached();
}
bool Writable() {
  const auto* owner = Owner();
  return owner && owner->writable();
}
bool SysContent(const std::string& name, std::string* content) {
  return NodeSysContent(kBlockName, name, content);
}
std::vector<std::string> Devices() {
  const auto* owner = Owner();
  return owner ? owner->devices() : std::vector<std::string>{};
}
bool DeviceExists(std::string_view name) {
  const auto* owner = Owner();
  return owner && owner->exists(name);
}
bool NodeSysContent(std::string_view node, const std::string& name, std::string* content) {
  const auto* owner = Owner();
  return owner && owner->content(node, name, content);
}
int Open(int flags) { return OpenNode(kBlockName, flags); }
int OpenNode(std::string_view node, int flags) {
  const auto* owner = Owner();
  return owner ? owner->open(node, flags) : -1;
}
int Stat(struct stat* info) { return StatNode(kBlockName, info); }
int StatNode(std::string_view node, struct stat* info) {
  const auto* owner = Owner();
  return owner ? owner->stat(node, info) : -1;
}
bool StatFd(const DescriptorSnapshotPtr& snapshot, struct stat* info, int* result) {
  const auto* owner = Owner();
  if (owner) return owner->stat_fd(snapshot, info, result);
  *result = -1;
  return snapshot && snapshot->description && snapshot->description->usb_block;
}
bool Io(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,
        bool writing, int64_t requested_offset, int* result) {
  const auto* owner = Owner();
  if (owner) return owner->io(snapshot, data, size, writing, requested_offset, result);
  *result = -1;
  return snapshot && snapshot->description && snapshot->description->usb_block;
}
bool Seek(const DescriptorSnapshotPtr& snapshot, int64_t offset, int whence, int64_t* result) {
  const auto* owner = Owner();
  if (owner) return owner->seek(snapshot, offset, whence, result);
  *result = -1;
  return snapshot && snapshot->description && snapshot->description->usb_block;
}
bool Sync(const DescriptorSnapshotPtr& snapshot, int* result) {
  const auto* owner = Owner();
  if (owner) return owner->sync(snapshot, result);
  *result = -1;
  return snapshot && snapshot->description && snapshot->description->usb_block;
}
bool Ioctl(const DescriptorSnapshotPtr& snapshot, unsigned long operation, void* output,
           int* result) {
  const auto* owner = Owner();
  if (owner) return owner->ioctl(snapshot, operation, output, result);
  *result = -1;
  return snapshot && snapshot->description && snapshot->description->usb_block;
}

}  // namespace gesher::usb_block
