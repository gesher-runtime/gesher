// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "host_disk.h"

#include <initguid.h>
#include <winioctl.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>

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

DWORD DiskAttributes(HANDLE disk, uint64_t* bits) {
  // The documented Win32 layout and control code are missing in mingw-w64.
  constexpr DWORD control = CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS);
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
  *bits = attributes.bits;
  return ERROR_SUCCESS;
}

DWORD VerifyOffline(HANDLE disk) {
  uint64_t attributes = 0;
  const DWORD error = DiskAttributes(disk, &attributes);
  if (error) return error;
  return (attributes & 1) ? ERROR_SUCCESS : ERROR_SHARING_VIOLATION;
}

DWORD PhysicallyWritable(HANDLE disk) {
  DWORD received = 0;
  return DeviceIoControl(disk, IOCTL_DISK_IS_WRITABLE, nullptr, 0,
                         nullptr, 0, &received, nullptr) ? ERROR_SUCCESS : GetLastError();
}

DWORD DiskNumber(HANDLE disk, DWORD* number) {
  STORAGE_DEVICE_NUMBER value{};
  DWORD received = 0;
  if (!DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
      &value, sizeof(value), &received, nullptr)) return GetLastError();
  if (received != sizeof(value) || value.DeviceType != FILE_DEVICE_DISK ||
      (value.PartitionNumber != 0 && value.PartitionNumber != MAXDWORD)) return ERROR_ACCESS_DENIED;
  *number = value.DeviceNumber;
  return ERROR_SUCCESS;
}

DWORD VolumeExtents(HANDLE volume, std::vector<DISK_EXTENT>* extents) {
  constexpr size_t kMaximumExtents = 128;
  alignas(VOLUME_DISK_EXTENTS) std::byte buffer[offsetof(VOLUME_DISK_EXTENTS, Extents) +
      sizeof(DISK_EXTENT) * kMaximumExtents]{};
  DWORD received = 0;
  if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
      buffer, sizeof(buffer), &received, nullptr)) return GetLastError();
  DWORD count = 0;
  std::memcpy(&count, buffer, sizeof(count));
  if (!count || count > kMaximumExtents || received != offsetof(VOLUME_DISK_EXTENTS, Extents) +
      sizeof(DISK_EXTENT) * count) return ERROR_INVALID_DATA;
  extents->resize(count);
  std::memcpy(extents->data(), buffer + offsetof(VOLUME_DISK_EXTENTS, Extents), sizeof(DISK_EXTENT) * count);
  return ERROR_SUCCESS;
}

DiskHandle OpenVolume(std::wstring name, DWORD access) {
  if (!name.empty() && name.back() == L'\\') name.pop_back();
  const HANDLE handle = CreateFileW(name.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, 0, nullptr);
  return DiskHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

DWORD RejectSystemDisk(DWORD number) {
  wchar_t windows[32768], root[32768], name[32768];
  const UINT size = GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
  if (!size || size >= std::size(windows)) return ERROR_INVALID_DATA;
  if (!GetVolumePathNameW(windows, root, static_cast<DWORD>(std::size(root))) ||
      !GetVolumeNameForVolumeMountPointW(root, name, static_cast<DWORD>(std::size(name)))) {
    return GetLastError();
  }
  auto volume = OpenVolume(name, 0);
  if (!volume) return GetLastError();
  std::vector<DISK_EXTENT> extents;
  const DWORD error = VolumeExtents(volume.get(), &extents);
  if (error) return error;
  return std::any_of(extents.begin(), extents.end(), [number](const auto& item) {
    return item.DiskNumber == number;
  }) ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}

DWORD VolumeIdentity(HANDLE volume, DWORD disk_number, std::wstring* identity) {
  if (GetFileType(volume) != FILE_TYPE_DISK) return ERROR_INVALID_HANDLE;
  std::vector<DISK_EXTENT> extents;
  DWORD error = VolumeExtents(volume, &extents);
  if (error) return error;
  // A spanned volume could contain data from another disk. Never lock or
  // dismount it on the strength of consent for this USB reader alone.
  if (extents.size() != 1 || extents.front().DiskNumber != disk_number ||
      extents.front().StartingOffset.QuadPart < 0 || extents.front().ExtentLength.QuadPart <= 0) {
    return ERROR_NOT_SUPPORTED;
  }
  // Mount manager's stable volume identity, plus its exact extent. A new
  // volume appearing on the disk invalidates the current exclusive session.
  constexpr DWORD query = CTL_CODE('M', 0, METHOD_BUFFERED, FILE_ANY_ACCESS);
  alignas(USHORT) BYTE bytes[4096]{};
  DWORD received = 0;
  if (!DeviceIoControl(volume, query, nullptr, 0, bytes, sizeof(bytes), &received, nullptr)) {
    return GetLastError();
  }
  USHORT length = 0;
  std::memcpy(&length, bytes, sizeof(length));
  if (!length || received < sizeof(length) || length > received - sizeof(length)) return ERROR_INVALID_DATA;
  static constexpr wchar_t hex[] = L"0123456789abcdef";
  identity->clear();
  for (size_t i = 0; i < length; ++i) {
    const BYTE value = bytes[sizeof(length) + i];
    identity->push_back(hex[value >> 4]);
    identity->push_back(hex[value & 15]);
  }
  *identity += L":" + std::to_wstring(extents.front().StartingOffset.QuadPart) +
               L":" + std::to_wstring(extents.front().ExtentLength.QuadPart);
  return ERROR_SUCCESS;
}

struct DiskVolume { std::wstring name; std::wstring identity; };

DWORD DiskVolumes(DWORD disk_number, std::vector<DiskVolume>* volumes) {
  volumes->clear();
  wchar_t name[32768];
  HANDLE search = FindFirstVolumeW(name, static_cast<DWORD>(std::size(name)));
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
  }
  const auto close_search = [](void* value) { FindVolumeClose(value); };
  const std::unique_ptr<void, decltype(close_search)> cleanup(search, close_search);
  for (;;) {
    auto volume = OpenVolume(name, 0);
    if (!volume) return GetLastError();
    std::vector<DISK_EXTENT> extents;
    DWORD error = VolumeExtents(volume.get(), &extents);
    // Non-disk providers (optical/network) do not supply disk extents.
    if (error != ERROR_SUCCESS && error != ERROR_INVALID_FUNCTION && error != ERROR_NOT_SUPPORTED &&
        error != ERROR_NOT_READY) return error;
    if (error == ERROR_SUCCESS && std::any_of(extents.begin(), extents.end(), [disk_number](const auto& extent) {
          return extent.DiskNumber == disk_number;
        })) {
      std::wstring identity;
      error = VolumeIdentity(volume.get(), disk_number, &identity);
      if (error) return error;
      if (volumes->size() >= 128) return ERROR_NOT_ENOUGH_QUOTA;
      volumes->push_back({name, std::move(identity)});
    }
    if (!FindNextVolumeW(search, name, static_cast<DWORD>(std::size(name)))) {
      const DWORD last = GetLastError();
      return last == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : last;
    }
  }
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
  if (!disk || (access != Access::kReadOnly && access != Access::kOfflineReadWrite)) return ERROR_INVALID_PARAMETER;
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
  auto selected = std::unique_ptr<HostDisk>(new HostDisk(handle.release()));
  DWORD status = selected->Inspect();
  if (status == ERROR_SUCCESS && writable) status = selected->PrepareWriteAccess();
  if (status == ERROR_SUCCESS && !writable) {
    const DWORD media_write = PhysicallyWritable(selected->handle_);
    if (media_write != ERROR_WRITE_PROTECT) status = media_write;
  }
  if (status == ERROR_SUCCESS) status = selected->Validate();
  if (status != ERROR_SUCCESS) return status;
  *disk = std::move(selected);
  return ERROR_SUCCESS;
}

DWORD HostDisk::Inspect() {
  DWORD status = DescribeUsb(handle_, &info_);
  if (status == ERROR_SUCCESS) status = DiskNumber(handle_, &disk_number_);
  if (status == ERROR_SUCCESS) status = RejectSystemDisk(disk_number_);
  if (status != ERROR_SUCCESS) return status;
  status = GetMediaGeneration(handle_, &media_generation_);
  if (info_.removable && status != ERROR_SUCCESS) return status;
  track_media_ = status == ERROR_SUCCESS;
  return Geometry(handle_, &info_);
}

DWORD HostDisk::PrepareWriteAccess() {
  if (!info_.removable || !track_media_) return ERROR_NOT_SUPPORTED;
  DWORD status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  status = SetMediaLock(handle_, true);
  if (status != ERROR_SUCCESS && status != ERROR_INVALID_FUNCTION && status != ERROR_NOT_SUPPORTED) return status;
  prevent_removal_ = status == ERROR_SUCCESS;
  status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  status = VerifyOffline(handle_);
  if (status != ERROR_SUCCESS) status = LockVolumes();
  if (status != ERROR_SUCCESS) return status;
  const DWORD media_write = PhysicallyWritable(handle_);
  if (media_write != ERROR_SUCCESS && media_write != ERROR_WRITE_PROTECT) return media_write;
  info_.write_consented = true;
  info_.writable = media_write == ERROR_SUCCESS;
  return ERROR_SUCCESS;
}

DWORD HostDisk::OpenSelectedForWrite(std::unique_ptr<HostDisk>* disk) {
  if (!disk || *disk) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  if (info_.write_consented || !info_.removable || !track_media_) return ERROR_NOT_SUPPORTED;
  DWORD status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  // ReOpenFile follows the retained handle, so a change to the enumerated
  // interface path cannot substitute another reader. No pathname fallback.
  HANDLE reopened = ReOpenFile(handle_, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, 0);
  if (reopened == INVALID_HANDLE_VALUE) return GetLastError();
  DiskHandle handle(reopened);
  auto selected = std::unique_ptr<HostDisk>(new HostDisk(handle.release()));
  status = selected->Inspect();
  if (status != ERROR_SUCCESS) return status;
  if (!selected->info_.removable || !selected->track_media_ ||
      selected->disk_number_ != disk_number_ || selected->media_generation_ != media_generation_ ||
      selected->info_.bytes != info_.bytes || selected->info_.sector_bytes != info_.sector_bytes) {
    return ERROR_MEDIA_CHANGED;
  }
  status = CheckMedia();
  if (status != ERROR_SUCCESS) return status;
  status = selected->PrepareWriteAccess();
  if (status != ERROR_SUCCESS) return status;
  status = selected->Validate();
  if (status != ERROR_SUCCESS) return status;
  *disk = std::move(selected);
  return ERROR_SUCCESS;
}

DWORD HostDisk::DuplicateForAuthority(HANDLE* result, HostDiskIdentity* identity,
                                     std::vector<HANDLE>* volume_handles) {
  if (!result || *result || !identity || (volume_handles && !volume_handles->empty())) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  DWORD error = CheckMedia();
  if (error == ERROR_SUCCESS && info_.write_consented) error = VerifyExclusion();
  if (error != ERROR_SUCCESS) return error;
  if (!volume_handles && !volume_handles_.empty()) return ERROR_INSUFFICIENT_BUFFER;
  std::vector<HANDLE> duplicates;
  duplicates.reserve(volume_handles_.size());
  for (HANDLE volume : volume_handles_) {
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), volume, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      error = GetLastError();
      for (HANDLE previous : duplicates) CloseHandle(previous);
      return error;
    }
    duplicates.push_back(copy);
  }
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), handle_, GetCurrentProcess(), &duplicate,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) {
    error = GetLastError();
    for (HANDLE previous : duplicates) CloseHandle(previous);
    return error;
  }
  *identity = {info_.bytes, info_.sector_bytes, media_generation_,
      (info_.removable ? 1u : 0u) | (info_.write_consented ? 2u : 0u) | (track_media_ ? 4u : 0u) |
      (prevent_removal_ ? 8u : 0u) | (volume_lock_mode_ ? 16u : 0u), static_cast<uint32_t>(duplicates.size())};
  if (volume_handles) *volume_handles = std::move(duplicates);
  *result = duplicate;
  return ERROR_SUCCESS;
}

DWORD HostDisk::Adopt(HANDLE owned_handle, Access access, const HostDiskIdentity& expected,
                      std::unique_ptr<HostDisk>* disk, std::vector<HANDLE> volume_handles) {
  DiskHandle handle(owned_handle == INVALID_HANDLE_VALUE ? nullptr : owned_handle);
  struct CloseVolumes {
    std::vector<HANDLE>& handles;
    ~CloseVolumes() { for (HANDLE volume : handles) if (volume && volume != INVALID_HANDLE_VALUE) CloseHandle(volume); }
  } cleanup{volume_handles};
  if (!disk || *disk || !handle || expected.volume_count != volume_handles.size() ||
      expected.volume_count > 128 || (expected.flags & ~31u) ||
      (access != Access::kReadOnly && access != Access::kOfflineReadWrite) ||
      ((expected.flags & 2u) != 0) != (access == Access::kOfflineReadWrite) ||
      (access == Access::kReadOnly && (expected.volume_count || (expected.flags & 24u)))) {
    return ERROR_INVALID_PARAMETER;
  }
  if (GetFileType(handle.get()) != FILE_TYPE_DISK) return ERROR_INVALID_HANDLE;
  STORAGE_DEVICE_NUMBER number{};
  DWORD received = 0;
  if (!DeviceIoControl(handle.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                       &number, sizeof(number), &received, nullptr)) return GetLastError();
  if (received != sizeof(number) || number.DeviceType != FILE_DEVICE_DISK ||
      (number.PartitionNumber != 0 && number.PartitionNumber != DWORD(-1))) return ERROR_ACCESS_DENIED;
  auto selected = std::unique_ptr<HostDisk>(new HostDisk(handle.release()));
  DWORD error = DescribeUsb(selected->handle_, &selected->info_);
  if (error == ERROR_SUCCESS) error = Geometry(selected->handle_, &selected->info_);
  if (error == ERROR_SUCCESS) error = DiskNumber(selected->handle_, &selected->disk_number_);
  if (error == ERROR_SUCCESS) error = RejectSystemDisk(selected->disk_number_);
  if (error != ERROR_SUCCESS) return error;
  if (selected->info_.bytes != expected.bytes || selected->info_.sector_bytes != expected.sector_bytes ||
      selected->info_.removable != ((expected.flags & 1u) != 0)) return ERROR_MEDIA_CHANGED;
  ULONG generation = 0;
  error = GetMediaGeneration(selected->handle_, &generation);
  if ((expected.flags & 4u) != 0) {
    if (error != ERROR_SUCCESS) return error;
    if (generation != expected.media_generation) return ERROR_MEDIA_CHANGED;
    selected->media_generation_ = generation;
    selected->track_media_ = true;
  } else if (selected->info_.removable || error == ERROR_SUCCESS) {
    return ERROR_INVALID_DATA;
  }
  selected->info_.write_consented = access == Access::kOfflineReadWrite;
  const DWORD media_write = PhysicallyWritable(selected->handle_);
  if (media_write != ERROR_SUCCESS && media_write != ERROR_WRITE_PROTECT) return media_write;
  selected->info_.writable = selected->info_.write_consented && media_write == ERROR_SUCCESS;
  if (selected->info_.write_consented) {
    if (!selected->info_.removable) return ERROR_NOT_SUPPORTED;
    selected->volume_lock_mode_ = (expected.flags & 16u) != 0;
    if (!selected->volume_lock_mode_ && expected.volume_count) return ERROR_INVALID_DATA;
    for (HANDLE volume : volume_handles) {
      std::wstring identity;
      error = VolumeIdentity(volume, selected->disk_number_, &identity);
      if (error) return error;
      selected->volume_names_.push_back(std::move(identity));
    }
    std::sort(selected->volume_names_.begin(), selected->volume_names_.end());
    if (std::adjacent_find(selected->volume_names_.begin(), selected->volume_names_.end()) !=
        selected->volume_names_.end()) return ERROR_INVALID_DATA;
    selected->volume_handles_ = std::move(volume_handles);
    volume_handles.clear();
    error = selected->VerifyExclusion();
    if (error) return error;
    // Ownership of the existing lock transfers only on successful adoption.
    // The trusted chooser disarms its unlock after the service acknowledges.
    selected->prevent_removal_ = (expected.flags & 8u) != 0;
  }
  error = selected->Validate();
  if (error != ERROR_SUCCESS) return error;
  *disk = std::move(selected);
  return ERROR_SUCCESS;
}

void HostDisk::ReleaseAfterAuthorityHandoff() {
  std::lock_guard lock(mutex_);
  prevent_removal_ = false;
}

HostDisk::~HostDisk() {
  // FSCTL_LOCK_VOLUME belongs to the open FILE_OBJECT. Closing these exact
  // handles releases it only after the chooser/service handoff's last copy.
  for (HANDLE volume : volume_handles_) CloseHandle(volume);
  if (prevent_removal_) SetMediaLock(handle_, false);
  CloseHandle(handle_);
}

DWORD HostDisk::Validate() {
  std::lock_guard lock(mutex_);
  const DWORD media = CheckMedia();
  if (media != ERROR_SUCCESS) return media;
  // The effective write grant must still match the pinned host session when its
  // description is published. A lost offline or writable state is an error.
  if (info_.write_consented) return VerifyExclusion();
  if (track_media_) return ERROR_SUCCESS;
  // A fixed USB disk may not supply a change sequence. It cannot swap media,
  // but a failed live status query still retires this handle permanently.
  const DWORD status = DescribeUsb(handle_, nullptr);
  if (status != ERROR_SUCCESS) disconnected_ = true;
  return status;
}

DWORD HostDisk::LockVolumes() {
  DWORD received = 0;
  std::vector<DiskVolume> volumes;
  DWORD error = DiskVolumes(disk_number_, &volumes);
  if (error) return error;
  volume_handles_.reserve(volumes.size());
  volume_names_.reserve(volumes.size());
  for (const auto& item : volumes) {
    auto volume = OpenVolume(item.name, GENERIC_READ | GENERIC_WRITE);
    if (!volume && (GetLastError() == ERROR_WRITE_PROTECT || GetLastError() == ERROR_ACCESS_DENIED)) {
      volume = OpenVolume(item.name, GENERIC_READ);
    }
    if (!volume) return GetLastError();
    std::wstring identity;
    error = VolumeIdentity(volume.get(), disk_number_, &identity);
    if (error) return error;
    if (identity != item.identity) return ERROR_MEDIA_CHANGED;
    error = CheckMedia();
    if (error != ERROR_SUCCESS) return error;
    // LOCK fails if a program still has files open. Do not forcibly dismount
    // a busy volume: preserve that application's data and return its real
    // sharing error to the chooser. A successful lock excludes future opens.
    if (!DeviceIoControl(volume.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &received, nullptr)) {
      return GetLastError();
    }
    // FSCTL_LOCK_VOLUME flushes the filesystem's cache before granting the
    // exclusive lock, including a read-only volume opened to clear its flag.
    error = CheckMedia();
    if (error != ERROR_SUCCESS) return error;
    if (!DeviceIoControl(volume.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &received, nullptr)) {
      return GetLastError();
    }
    volume_names_.push_back(std::move(identity));
    volume_handles_.push_back(volume.release());
  }
  std::sort(volume_names_.begin(), volume_names_.end());
  if (std::adjacent_find(volume_names_.begin(), volume_names_.end()) != volume_names_.end()) {
    return ERROR_INVALID_DATA;
  }
  volume_lock_mode_ = true;
  return VerifyExclusion();
}

DWORD HostDisk::VerifyExclusion() {
  if (!volume_lock_mode_) return VerifyOffline(handle_);
  if (volume_names_.size() != volume_handles_.size()) return ERROR_INVALID_DATA;
  // The trusted chooser imported these exact still-open volume objects. They
  // are never exposed to an application and no code issues UNLOCK_VOLUME.
  // Check the retained object identities and the full live volume set before
  // every write, including RMW, so repartition/rescan cannot add an unheld FS.
  std::vector<std::wstring> retained;
  retained.reserve(volume_handles_.size());
  for (HANDLE volume : volume_handles_) {
    std::wstring identity;
    const DWORD error = VolumeIdentity(volume, disk_number_, &identity);
    if (error) return error;
    retained.push_back(std::move(identity));
  }
  std::sort(retained.begin(), retained.end());
  if (retained != volume_names_) return ERROR_MEDIA_CHANGED;
  std::vector<DiskVolume> volumes;
  const DWORD error = DiskVolumes(disk_number_, &volumes);
  if (error) return error;
  std::vector<std::wstring> live;
  live.reserve(volumes.size());
  for (auto& volume : volumes) live.push_back(std::move(volume.identity));
  std::sort(live.begin(), live.end());
  return live == volume_names_ ? ERROR_SUCCESS : ERROR_SHARING_VIOLATION;
}

DWORD HostDisk::VerifyWriteAccess() {
  if (!info_.write_consented) return ERROR_ACCESS_DENIED;
  const DWORD exclusive = VerifyExclusion();
  return exclusive ? exclusive : PhysicallyWritable(handle_);
}

DWORD HostDisk::Describe(DiskInfo* info) {
  if (!info) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  DWORD error = CheckMedia();
  if (!error && info_.write_consented) error = VerifyExclusion();
  if (error) return error;
  *info = info_;
  if (info_.write_consented) {
    error = PhysicallyWritable(handle_);
    if (error != ERROR_SUCCESS && error != ERROR_WRITE_PROTECT) return error;
    info->writable = error == ERROR_SUCCESS;
  }
  return ERROR_SUCCESS;
}

DWORD HostDisk::SetReadOnly(bool readonly) {
  std::lock_guard lock(mutex_);
  if (!info_.write_consented) return ERROR_ACCESS_DENIED;
  DWORD error = CheckMedia();
  if (!error) error = VerifyExclusion();
  if (error) return error;
  uint64_t attributes = 0;
  error = DiskAttributes(handle_, &attributes);
  if (error) {
    // A reader without software attributes can still confirm the requested
    // writable state when it already exists. A hardware write lock remains an
    // actual write-protect error; it is never cleared or hidden in metadata.
    if (!readonly && (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED)) {
      return PhysicallyWritable(handle_);
    }
    return error;
  }
  if (((attributes & 2) != 0) != readonly) {
    struct SetAttributes {
      DWORD version;
      BOOLEAN persist;
      BYTE reserved1[3];
      uint64_t bits;
      uint64_t mask;
      DWORD reserved2[4];
    } request{sizeof(SetAttributes), FALSE, {}, readonly ? uint64_t{2} : 0, 2, {}};
    static_assert(sizeof(SetAttributes) == 40);
    constexpr DWORD control = CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS);
    DWORD received = 0;
    if (!DeviceIoControl(handle_, control, &request, sizeof(request), nullptr, 0, &received, nullptr)) {
      return GetLastError();
    }
    error = DiskAttributes(handle_, &attributes);
    if (error) return error;
    if (((attributes & 2) != 0) != readonly) return ERROR_WRITE_PROTECT;
  }
  error = CheckMedia();
  if (error) return error;
  return readonly ? ERROR_SUCCESS : PhysicallyWritable(handle_);
}

DWORD HostDisk::Flush() {
  std::lock_guard lock(mutex_);
  DWORD error = CheckMedia();
  if (error) return error;
  if (!info_.write_consented) return ERROR_SUCCESS;  // This session cannot dirty the medium.
  error = VerifyExclusion();
  if (error) return error;
  return FlushFileBuffers(handle_) ? ERROR_SUCCESS : GetLastError();
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
  if (!info_.write_consented) return ERROR_ACCESS_DENIED;
  if (!ValidRange(offset, input.size())) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(mutex_);
  DWORD status = VerifyWriteAccess();
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
  status = VerifyWriteAccess();
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
