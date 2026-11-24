// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "host_disk.h"

#include <initguid.h>
#include <winioctl.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace gesher::usb_storage {
namespace {

struct CloseDisk {
  void operator()(void* handle) const { CloseHandle(handle); }
};
using DiskHandle = std::unique_ptr<void, CloseDisk>;

DiskHandle OpenDisk(const wchar_t* path, DWORD access) {
  // A writable session needs RMW exclusivity against another data handle.
  // Read-only lookups keep the normal disk-driver sharing behavior.
  const DWORD sharing = (access & GENERIC_WRITE) != 0 ? FILE_SHARE_READ
                                                     : FILE_SHARE_READ | FILE_SHARE_WRITE;
  HANDLE handle = CreateFileW(path, access, sharing, nullptr, OPEN_EXISTING, 0, nullptr);
  return DiskHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

CONFIGRET EnumerateDiskPaths(std::vector<wchar_t>* paths) {
  GUID disk_interface = GUID_DEVINTERFACE_DISK;
  for (int attempt = 0; attempt < 4; ++attempt) {
    ULONG size = 0;
    constexpr ULONG flags = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
    CONFIGRET result = CM_Get_Device_Interface_List_SizeW(
        &size, &disk_interface, nullptr, flags);
    if (result != CR_SUCCESS) return result;
    if (size == 0) return CR_INVALID_DATA;
    paths->resize(size);
    result = CM_Get_Device_Interface_ListW(
        &disk_interface, nullptr, paths->data(), size, flags);
    if (result != CR_BUFFER_SMALL) return result;
  }
  return CR_BUFFER_SMALL;
}

std::string DescriptorText(const std::vector<char>& bytes, DWORD offset,
                           size_t descriptor_end) {
  if (offset < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) ||
      offset >= descriptor_end) return {};
  const char* first = bytes.data() + offset;
  const char* last = bytes.data() + descriptor_end;
  const char* end = std::find(first, last, '\0');
  if (end == last) return {};
  std::string value(first, end);
  // The storage descriptor stores ASCII. Replace bytes outside printable ASCII
  // before carrying the string as UTF-8 or exposing it as sysfs text.
  for (char& ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 32 || byte > 126) ch = '?';
  }
  return value;
}

DWORD DescribeUsb(HANDLE disk, DiskInfo* info) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_DESCRIPTOR_HEADER header{};
  DWORD received = 0;
  if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                       &header, sizeof(header), &received, nullptr)) {
    return GetLastError();
  }
  constexpr auto required = offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties);
  if (received < sizeof(header) || header.Size < required || header.Size > 64 * 1024) {
    return ERROR_INVALID_DATA;
  }
  std::vector<char> bytes(header.Size);
  if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                       bytes.data(), header.Size, &received, nullptr)) {
    return GetLastError();
  }
  if (received < required || received > bytes.size()) return ERROR_INVALID_DATA;
  STORAGE_DEVICE_DESCRIPTOR descriptor{};
  std::memcpy(&descriptor, bytes.data(), required);
  if (descriptor.BusType != BusTypeUsb) return ERROR_NOT_SUPPORTED;
  if (info) {
    info->removable = descriptor.RemovableMedia != FALSE;
    info->model = DescriptorText(bytes, descriptor.ProductIdOffset, received);
    info->serial = DescriptorText(bytes, descriptor.SerialNumberOffset, received);
  }
  return ERROR_SUCCESS;
}

DWORD Geometry(HANDLE disk, DiskInfo* info) {
  alignas(DISK_GEOMETRY_EX) std::byte bytes[1024]{};
  DWORD received = 0;
  if (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                       bytes, sizeof(bytes), &received, nullptr)) {
    return GetLastError();
  }
  constexpr auto required = offsetof(DISK_GEOMETRY_EX, Data);
  if (received < required) return ERROR_INVALID_DATA;
  DISK_GEOMETRY_EX geometry{};
  std::memcpy(&geometry, bytes, required);
  const DWORD sector = geometry.Geometry.BytesPerSector;
  if (geometry.DiskSize.QuadPart <= 0 || sector < 512 || sector > 65536 ||
      (sector & (sector - 1)) != 0 || geometry.DiskSize.QuadPart % sector != 0) {
    return ERROR_INVALID_DATA;
  }
  info->bytes = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
  info->sector_bytes = sector;
  return ERROR_SUCCESS;
}

DWORD VerifyOfflineWritable(HANDLE disk) {
  // The documented Win32 layout and control code are missing in mingw-w64.
  constexpr DWORD control = CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS);
  constexpr uint64_t offline = 1;
  constexpr uint64_t read_only = 2;
  struct Attributes {
    DWORD version;
    DWORD reserved;
    uint64_t bits;
  } attributes{};
  static_assert(sizeof(Attributes) == 16);
  DWORD received = 0;
  if (!DeviceIoControl(disk, control, nullptr, 0,
                       &attributes, sizeof(attributes), &received, nullptr)) {
    return GetLastError();
  }
  if (received < sizeof(attributes) || attributes.version != sizeof(attributes)) {
    return ERROR_INVALID_DATA;
  }
  if ((attributes.bits & offline) == 0) return ERROR_SHARING_VIOLATION;
  if ((attributes.bits & read_only) != 0) return ERROR_WRITE_PROTECT;
  return DeviceIoControl(disk, IOCTL_DISK_IS_WRITABLE, nullptr, 0,
                         nullptr, 0, &received, nullptr) ? ERROR_SUCCESS : GetLastError();
}

DWORD GetMediaGeneration(HANDLE disk, ULONG* generation) {
  DWORD received = 0;
  if (!DeviceIoControl(disk, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0,
                       generation, sizeof(*generation), &received, nullptr)) {
    return GetLastError();
  }
  return received == sizeof(*generation) ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}

DWORD SetMediaLock(HANDLE disk, bool prevent) {
  PREVENT_MEDIA_REMOVAL request{};
  request.PreventMediaRemoval = prevent;
  DWORD received = 0;
  return DeviceIoControl(disk, IOCTL_STORAGE_EJECTION_CONTROL, &request, sizeof(request),
                         nullptr, 0, &received, nullptr) ? ERROR_SUCCESS : GetLastError();
}

DWORD PositionDisk(HANDLE disk, uint64_t offset) {
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  return SetFilePointerEx(disk, position, nullptr, FILE_BEGIN) ? ERROR_SUCCESS : GetLastError();
}

struct AlignedRange {
  uint64_t start;
  DWORD length;
  DWORD prefix;
};

constexpr AlignedRange Align(uint64_t start, size_t size, DWORD sector) {
  const auto prefix = static_cast<DWORD>(start % sector);
  const auto span = static_cast<DWORD>(prefix + size);
  return {start - prefix, (span + sector - 1) / sector * sector, prefix};
}

static_assert(Align(513, 3, 512).start == 512 && Align(513, 3, 512).prefix == 1);
static_assert(Align(4095, 2, 4096).length == 8192);
static_assert(Align(512, 1024, 512).length == 1024);

DWORD ReadDisk(HANDLE disk, AlignedRange range, void* buffer) {
  DWORD status = PositionDisk(disk, range.start);
  if (status != ERROR_SUCCESS) return status;
  DWORD received = 0;
  if (!ReadFile(disk, buffer, range.length, &received, nullptr)) return GetLastError();
  return received == range.length ? ERROR_SUCCESS : ERROR_READ_FAULT;
}

struct FreeBuffer {
  void operator()(void* buffer) const { VirtualFree(buffer, 0, MEM_RELEASE); }
};
using TransferBuffer = std::unique_ptr<void, FreeBuffer>;

}  // namespace

DWORD ListDisks(std::vector<std::wstring>* paths) {
  paths->clear();
  std::vector<wchar_t> interfaces;
  CONFIGRET result = EnumerateDiskPaths(&interfaces);
  if (result != CR_SUCCESS) return CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
  for (const wchar_t* path = interfaces.data(); *path; path += std::wcslen(path) + 1) {
    DiskHandle handle = OpenDisk(path, 0);
    if (handle && DescribeUsb(handle.get(), nullptr) == ERROR_SUCCESS) paths->emplace_back(path);
  }
  return ERROR_SUCCESS;
}

DWORD HostDisk::Open(const std::wstring& path, Access access,
                     std::unique_ptr<HostDisk>* disk) {
  disk->reset();
  if (path.empty() || path.find(L'\0') != std::wstring::npos) return ERROR_INVALID_NAME;
  std::vector<wchar_t> interfaces;
  CONFIGRET result = EnumerateDiskPaths(&interfaces);
  if (result != CR_SUCCESS) return CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
  const wchar_t* match = nullptr;
  for (const wchar_t* current = interfaces.data(); *current; current += std::wcslen(current) + 1) {
    if (CompareStringOrdinal(current, -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) {
      match = current;
      break;
    }
  }
  if (!match) return ERROR_NOT_FOUND;

  // Resolve only enumerated disk interfaces and reject non-USB devices before
  // asking Windows for data access. Verify again on the actual data handle.
  {
    auto inspect = OpenDisk(match, 0);
    if (!inspect) return GetLastError();
    const DWORD status = DescribeUsb(inspect.get(), nullptr);
    if (status != ERROR_SUCCESS) return status;
  }
  const bool writable = access == Access::kOfflineReadWrite;
  auto handle = OpenDisk(match, GENERIC_READ | (writable ? GENERIC_WRITE : 0));
  if (!handle) return GetLastError();
  auto selected = std::unique_ptr<HostDisk>(new HostDisk(handle.get()));
  handle.release();
  DWORD status = DescribeUsb(selected->handle_, &selected->info_);
  if (status != ERROR_SUCCESS) return status;
  if (writable) {
    if (!selected->info_.removable) return ERROR_NOT_SUPPORTED;
    status = VerifyOfflineWritable(selected->handle_);
    if (status != ERROR_SUCCESS) return status;
    status = SetMediaLock(selected->handle_, true);
    if (status != ERROR_SUCCESS) return status;
    selected->prevent_removal_ = true;
  }
  status = GetMediaGeneration(selected->handle_, &selected->media_generation_);
  if (selected->info_.removable && status != ERROR_SUCCESS) return status;
  selected->track_media_ = status == ERROR_SUCCESS;
  status = Geometry(selected->handle_, &selected->info_);
  if (status != ERROR_SUCCESS) return status;
  selected->info_.writable = writable;
  status = selected->Validate();
  if (status != ERROR_SUCCESS) return status;
  *disk = std::move(selected);
  return ERROR_SUCCESS;
}

HostDisk::~HostDisk() {
  if (prevent_removal_) SetMediaLock(handle_, false);
  CloseHandle(handle_);
}

DWORD HostDisk::Validate() {
  std::lock_guard lock(mutex_);
  const DWORD media = CheckMedia();
  if (media != ERROR_SUCCESS) return media;
  // The effective write grant must still match the pinned host session when its
  // description is published. A lost offline or writable state is an error.
  if (info_.writable) return VerifyOfflineWritable(handle_);
  if (track_media_) return ERROR_SUCCESS;
  // A fixed USB disk may not supply a change sequence. It cannot swap media,
  // but a failed live status query still retires this handle permanently.
  const DWORD status = DescribeUsb(handle_, nullptr);
  if (status != ERROR_SUCCESS) disconnected_ = true;
  return status;
}

DWORD HostDisk::CheckMedia() {
  if (disconnected_) return ERROR_MEDIA_CHANGED;
  if (!track_media_) return ERROR_SUCCESS;
  ULONG current = 0;
  DWORD status = GetMediaGeneration(handle_, &current);
  if (status != ERROR_SUCCESS || current != media_generation_) {
    disconnected_ = true;
    return status != ERROR_SUCCESS ? status : ERROR_MEDIA_CHANGED;
  }
  return ERROR_SUCCESS;
}

bool HostDisk::ValidRange(uint64_t offset, size_t size) const {
  return size != 0 && size <= kMaxTransfer && offset <= info_.bytes &&
         size <= info_.bytes - offset;
}

DWORD HostDisk::Read(uint64_t offset, std::span<std::byte> output) {
  if (!ValidRange(offset, output.size())) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  DWORD status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  const AlignedRange range = Align(offset, output.size(), info_.sector_bytes);
  TransferBuffer buffer(VirtualAlloc(nullptr, range.length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!buffer) return GetLastError();
  status = ReadDisk(handle_, range, buffer.get());
  if (status != ERROR_SUCCESS) return status;
  status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  std::memcpy(output.data(), static_cast<std::byte*>(buffer.get()) + range.prefix, output.size());
  return ERROR_SUCCESS;
}

DWORD HostDisk::Write(uint64_t offset, std::span<const std::byte> input) {
  if (!info_.writable) return ERROR_ACCESS_DENIED;
  if (!ValidRange(offset, input.size())) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  DWORD status = VerifyOfflineWritable(handle_);
  if (status != ERROR_SUCCESS) return status;
  status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  const AlignedRange range = Align(offset, input.size(), info_.sector_bytes);
  TransferBuffer buffer(VirtualAlloc(nullptr, range.length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!buffer) return GetLastError();
  if (input.size() != range.length) {
    status = ReadDisk(handle_, range, buffer.get());
    if (status != ERROR_SUCCESS) return status;
  }
  std::memcpy(static_cast<std::byte*>(buffer.get()) + range.prefix, input.data(), input.size());
  status = VerifyOfflineWritable(handle_);
  if (status != ERROR_SUCCESS) return status;
  status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  status = PositionDisk(handle_, range.start);
  if (status != ERROR_SUCCESS) return status;
  DWORD sent = 0;
  if (!WriteFile(handle_, buffer.get(), range.length, &sent, nullptr)) {
    return GetLastError();
  }
  if (sent != range.length) return ERROR_WRITE_FAULT;
  return FlushFileBuffers(handle_) ? ERROR_SUCCESS : GetLastError();
}

}  // namespace gesher::usb_storage
