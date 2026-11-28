// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "../../compat/usb_storage/host_disk.h"

#include <cfgmgr32.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
using gesher::usb_storage::Access;
using gesher::usb_storage::HostDisk;

struct CloseMetadataHandle {
  void operator()(void* handle) const { CloseHandle(handle); }
};

DWORD PresentDiskPaths(std::vector<std::wstring>* paths) {
  GUID disk_interface = GUID_DEVINTERFACE_DISK;
  constexpr ULONG flags = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
  for (int attempt = 0; attempt < 4; ++attempt) {
    ULONG size = 0;
    CONFIGRET result = CM_Get_Device_Interface_List_SizeW(
        &size, &disk_interface, nullptr, flags);
    if (result != CR_SUCCESS) return CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
    if (!size || size > 1024 * 1024) return ERROR_INVALID_DATA;
    std::vector<wchar_t> interfaces(size);
    result = CM_Get_Device_Interface_ListW(
        &disk_interface, nullptr, interfaces.data(), size, flags);
    if (result == CR_BUFFER_SMALL) continue;
    if (result != CR_SUCCESS) return CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
    if (interfaces.back() != L'\0') return ERROR_INVALID_DATA;
    for (auto first = interfaces.begin(); first != interfaces.end() && *first;) {
      const auto last = std::find(first, interfaces.end(), L'\0');
      if (last == interfaces.end()) return ERROR_INVALID_DATA;
      paths->emplace_back(first, last);
      first = last + 1;
    }
    return ERROR_SUCCESS;
  }
  return ERROR_INSUFFICIENT_BUFFER;
}

DWORD QueryBus(const std::wstring& path, STORAGE_BUS_TYPE* bus) {
  // Zero desired access: this handle is only for storage-descriptor metadata.
  // This test never requests writable access or reads disk contents.
  HANDLE raw = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
  if (raw == INVALID_HANDLE_VALUE) return GetLastError();
  const std::unique_ptr<void, CloseMetadataHandle> handle(raw);
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_DESCRIPTOR_HEADER header{};
  DWORD received = 0;
  if (!DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY,
                       &query, sizeof(query), &header, sizeof(header),
                       &received, nullptr)) return GetLastError();
  constexpr size_t minimum = offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties);
  if (received < sizeof(header) || header.Size < minimum || header.Size > 64 * 1024) {
    return ERROR_INVALID_DATA;
  }
  std::vector<std::byte> bytes(header.Size);
  if (!DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY,
                       &query, sizeof(query), bytes.data(), header.Size,
                       &received, nullptr)) return GetLastError();
  if (received < minimum || received > bytes.size()) return ERROR_INVALID_DATA;
  STORAGE_DEVICE_DESCRIPTOR descriptor{};
  std::memcpy(&descriptor, bytes.data(), minimum);
  *bus = descriptor.BusType;
  return ERROR_SUCCESS;
}
}  // namespace

int main() {
  std::vector<std::wstring> present;
  DWORD error = PresentDiskPaths(&present);
  if (error != ERROR_SUCCESS) {
    std::fprintf(stderr, "FAIL internal-disk admission: present-interface enumeration error=%lu\n",
                 static_cast<unsigned long>(error));
    return 1;
  }
  std::vector<std::wstring> offered;
  error = gesher::usb_storage::ListDisks(&offered);
  if (error != ERROR_SUCCESS) {
    std::fprintf(stderr, "FAIL internal-disk admission: production ListDisks error=%lu\n",
                 static_cast<unsigned long>(error));
    return 1;
  }

  size_t checked = 0;
  size_t unavailable = 0;
  size_t unknown = 0;
  DWORD last_error = ERROR_SUCCESS;
  for (const auto& path : present) {
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    error = QueryBus(path, &bus);
    if (error != ERROR_SUCCESS) {
      ++unavailable;
      last_error = error;
      std::fprintf(stderr, "Internal-disk descriptor unavailable: error=%lu\n",
                   static_cast<unsigned long>(error));
      continue;
    }
    if (bus == BusTypeUsb) continue;
    if (bus <= BusTypeUnknown || bus >= BusTypeMax) {
      ++unknown;
      continue;
    }
    const auto selected = std::find_if(offered.begin(), offered.end(), [&](const auto& item) {
      return CompareStringOrdinal(path.c_str(), -1, item.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (selected != offered.end()) {
      std::fprintf(stderr, "FAIL internal-disk admission: production ListDisks offered non-USB bus=%u\n",
                   static_cast<unsigned>(bus));
      return 1;
    }
    std::unique_ptr<HostDisk> disk;
    error = HostDisk::Open(path, Access::kReadOnly, &disk);
    if (error != ERROR_NOT_SUPPORTED || disk) {
      std::fprintf(stderr,
          "FAIL internal-disk admission: non-USB bus=%u Open error=%lu expected=%lu object=%s\n",
          static_cast<unsigned>(bus), static_cast<unsigned long>(error),
          static_cast<unsigned long>(ERROR_NOT_SUPPORTED), disk ? "present" : "null");
      return 1;
    }
    ++checked;
    std::printf("PASS real non-USB disk: bus=%u excluded by ListDisks; read-only Open=%lu object=null\n",
                static_cast<unsigned>(bus), static_cast<unsigned long>(error));
  }
  if (!checked) {
    std::fprintf(stderr,
        "FAIL internal-disk admission unavailable: no present disk with a readable, known non-USB bus; "
        "present=%zu descriptor_errors=%zu unknown_bus=%zu last_error=%lu. "
        "A real non-USB disk is required; no fixture substitutes for this check.\n",
        present.size(), unavailable, unknown, static_cast<unsigned long>(last_error));
    return 1;
  }
  std::printf("Real non-USB disk admission: %zu disks rejected by unchanged production code; "
              "metadata queries only, no disk data I/O or media-state changes\n", checked);
  return 0;
}
