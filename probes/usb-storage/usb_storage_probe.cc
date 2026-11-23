// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <windows.h>
#include <initguid.h>
#include <winioctl.h>
#include <cfgmgr32.h>

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kMaxRead = 1024 * 1024;

struct CloseDisk {
  void operator()(void* handle) const { CloseHandle(handle); }
};
using DiskHandle = std::unique_ptr<void, CloseDisk>;

DiskHandle OpenDisk(const wchar_t* path, DWORD access) {
  HANDLE handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
  return DiskHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

CONFIGRET EnumerateDiskPaths(std::vector<wchar_t>& paths) {
  GUID disk_interface = GUID_DEVINTERFACE_DISK;
  for (int attempt = 0; attempt < 4; ++attempt) {
    ULONG size = 0;
    constexpr ULONG flags = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
    CONFIGRET result = CM_Get_Device_Interface_List_SizeW(
        &size, &disk_interface, nullptr, flags);
    if (result != CR_SUCCESS) return result;
    if (size == 0) return CR_INVALID_DATA;

    paths.resize(size);
    result = CM_Get_Device_Interface_ListW(
        &disk_interface, nullptr, paths.data(), size, flags);
    if (result != CR_BUFFER_SMALL) return result;
  }
  return CR_BUFFER_SMALL;
}

DWORD CheckUsbBus(HANDLE disk) {
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

  std::vector<std::byte> bytes(header.Size);
  if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                       bytes.data(), header.Size, &received, nullptr)) {
    return GetLastError();
  }
  if (received < required) return ERROR_INVALID_DATA;
  STORAGE_DEVICE_DESCRIPTOR descriptor{};
  std::memcpy(&descriptor, bytes.data(), required);
  return descriptor.BusType == BusTypeUsb ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}

struct FreeBuffer {
  void operator()(void* buffer) const { VirtualFree(buffer, 0, MEM_RELEASE); }
};

DWORD ReadSectors(HANDLE disk, unsigned long long offset, DWORD length) {
  // DISK_GEOMETRY_EX has optional trailing partition and detection information.
  alignas(DISK_GEOMETRY_EX) std::byte geometry_bytes[1024]{};
  DWORD received = 0;
  if (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                       geometry_bytes, sizeof(geometry_bytes), &received, nullptr)) {
    return GetLastError();
  }
  constexpr auto required = offsetof(DISK_GEOMETRY_EX, Data);
  if (received < required) return ERROR_INVALID_DATA;
  DISK_GEOMETRY_EX geometry{};
  std::memcpy(&geometry, geometry_bytes, required);
  const DWORD sector = geometry.Geometry.BytesPerSector;
  if (geometry.DiskSize.QuadPart <= 0 || sector == 0 || sector > 65536 ||
      (sector & (sector - 1)) != 0) {
    return ERROR_INVALID_DATA;
  }
  const auto size = static_cast<unsigned long long>(geometry.DiskSize.QuadPart);
  if (length == 0 || length > kMaxRead || offset % sector != 0 ||
      length % sector != 0 || offset > size || length > size - offset) {
    return ERROR_INVALID_PARAMETER;
  }

  std::unique_ptr<void, FreeBuffer> buffer(
      VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!buffer) return GetLastError();
  LARGE_INTEGER start{};
  start.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(disk, start, nullptr, FILE_BEGIN)) return GetLastError();
  if (!ReadFile(disk, buffer.get(), length, &received, nullptr)) return GetLastError();
  if (received != length) return ERROR_READ_FAULT;
  if (_setmode(_fileno(stdout), _O_BINARY) < 0 ||
      std::fwrite(buffer.get(), 1, length, stdout) != length || std::fflush(stdout) != 0) {
    return ERROR_WRITE_FAULT;
  }
  return ERROR_SUCCESS;
}

bool ParseNumber(std::wstring_view argument, unsigned long long& value) {
  char ascii[20];
  if (argument.empty() || argument.size() > sizeof(ascii) ||
      argument.find_first_not_of(L"0123456789") != std::wstring_view::npos) {
    return false;
  }
  for (size_t i = 0; i < argument.size(); ++i) {
    ascii[i] = static_cast<char>(argument[i]);
  }
  return std::from_chars(ascii, ascii + argument.size(), value).ec == std::errc{};
}

int Fail(const wchar_t* operation, DWORD error) {
  std::fwprintf(stderr, L"%ls failed (Windows error %lu)\n", operation, error);
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool list = argc == 2 && std::wstring_view(argv[1]) == L"--list";
  unsigned long long offset = 0;
  unsigned long long length = 0;
  if (!list && (argc != 5 || std::wstring_view(argv[1]) != L"--read" ||
                !ParseNumber(argv[3], offset) || !ParseNumber(argv[4], length) ||
                offset > static_cast<unsigned long long>(std::numeric_limits<LONGLONG>::max()) ||
                length == 0 || length > kMaxRead)) {
    std::fwprintf(stderr, L"Usage: usb_storage_probe --list | --read <device-path> <offset> <bytes>\n");
    return 2;
  }

  std::vector<wchar_t> paths;
  const CONFIGRET result = EnumerateDiskPaths(paths);
  if (result != CR_SUCCESS) return Fail(L"Disk enumeration", CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE));
  if (list && _setmode(_fileno(stdout), _O_U8TEXT) < 0) {
    return Fail(L"Device list output", ERROR_WRITE_FAULT);
  }
  for (const wchar_t* path = paths.data(); *path; path += std::wcslen(path) + 1) {
    if (!list && CompareStringOrdinal(path, -1, argv[2], -1, TRUE) != CSTR_EQUAL) continue;

    auto disk = OpenDisk(path, 0);
    DWORD error = disk ? CheckUsbBus(disk.get()) : GetLastError();
    if (list) {
      if (error == ERROR_SUCCESS && std::wprintf(L"%ls\n", path) < 0) {
        return Fail(L"Device list output", ERROR_WRITE_FAULT);
      }
      continue;
    }
    if (error != ERROR_SUCCESS) return Fail(L"USB storage identification", error);
    disk = OpenDisk(path, GENERIC_READ);
    if (!disk) return Fail(L"Read-only USB storage open", GetLastError());
    error = CheckUsbBus(disk.get());
    if (error == ERROR_SUCCESS) {
      error = ReadSectors(disk.get(), offset, static_cast<DWORD>(length));
    }
    return error == ERROR_SUCCESS ? 0 : Fail(L"USB storage read", error);
  }
  if (!list) return Fail(L"Present disk interface lookup", ERROR_NOT_FOUND);
  return std::fflush(stdout) == 0 ? 0 : Fail(L"Device list output", ERROR_WRITE_FAULT);
}
