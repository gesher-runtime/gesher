// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "usb_storage_observation.h"

#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <usbioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <set>

#include "usb_winusb.h"

namespace gesher::usb {
namespace {

// Standard GUID_DEVINTERFACE_USB_DEVICE and GUID_DEVINTERFACE_USB_HUB.
constexpr GUID kUsbDevice{0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
constexpr GUID kUsbHub{0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};

struct DestroyDeviceSet {
  void operator()(void* set) const { SetupDiDestroyDeviceInfoList(set); }
};

struct Interface {
  std::wstring path;
  std::wstring driver;
};

std::wstring Property(HDEVINFO devices, SP_DEVINFO_DATA* devnode, DWORD property) {
  wchar_t value[512]{};
  DWORD type = 0;
  if (!SetupDiGetDeviceRegistryPropertyW(devices, devnode, property, &type,
                                        reinterpret_cast<BYTE*>(value),
                                        sizeof(value) - sizeof(value[0]), nullptr) ||
      type != REG_SZ) return {};
  return value;
}

std::optional<std::vector<Interface>> Interfaces(const GUID& guid, bool storage) {
  const HDEVINFO raw = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                          DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
  if (raw == INVALID_HANDLE_VALUE) return std::nullopt;
  const std::unique_ptr<void, DestroyDeviceSet> devices(raw);
  std::vector<Interface> result;
  SP_DEVICE_INTERFACE_DATA entry{};
  entry.cbSize = sizeof(entry);
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(raw, nullptr, &guid, i, &entry); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(raw, &entry, nullptr, 0, &size, nullptr);
    if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size > 32768) continue;
    std::vector<unsigned char> buffer(size, 0);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA devnode{};
    devnode.cbSize = sizeof(devnode);
    if (!SetupDiGetDeviceInterfaceDetailW(raw, &entry, detail, size, nullptr, &devnode)) continue;
    const size_t max_path = (size - offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath))
        / sizeof(wchar_t);
    const wchar_t* end = std::wmemchr(detail->DevicePath, L'\0', max_path);
    if (!end || end == detail->DevicePath) continue;
    std::wstring driver;
    if (storage) {
      const auto service = Property(raw, &devnode, SPDRP_SERVICE);
      if (_wcsicmp(service.c_str(), L"USBSTOR") != 0 &&
          _wcsicmp(service.c_str(), L"UASPStor") != 0) continue;
      driver = Property(raw, &devnode, SPDRP_DRIVER);
      if (driver.empty()) continue;
    }
    result.push_back({std::wstring(static_cast<const wchar_t*>(detail->DevicePath), end),
                      std::move(driver)});
  }
  if (GetLastError() != ERROR_NO_MORE_ITEMS) return std::nullopt;
  return result;
}

std::wstring PortDriver(HANDLE hub, ULONG port) {
  constexpr size_t start = offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName);
  alignas(USB_NODE_CONNECTION_DRIVERKEY_NAME) std::array<unsigned char, 2048> bytes{};
  auto* request = reinterpret_cast<USB_NODE_CONNECTION_DRIVERKEY_NAME*>(bytes.data());
  request->ConnectionIndex = port;
  DWORD returned = 0;
  if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME,
                       request, sizeof(*request), request, bytes.size(), &returned, nullptr) ||
      returned < start + sizeof(wchar_t) || returned > bytes.size() ||
      request->ActualLength != returned) return {};
  const size_t size = (returned - start) / sizeof(wchar_t);
  const wchar_t* end = std::wmemchr(request->DriverKeyName, L'\0', size);
  return end ? std::wstring(static_cast<const wchar_t*>(request->DriverKeyName), end) : L"";
}

struct PortState {
  USB_DEVICE_DESCRIPTOR descriptor{};
  USHORT address = 0;
  UCHAR configuration = 0;
};

std::optional<PortState> ConnectedPort(HANDLE hub, ULONG port) {
  struct PortBuffer {
    USB_NODE_CONNECTION_INFORMATION_EX connection{};
    USB_PIPE_INFO extra[30]{};
  } data;
  data.connection.ConnectionIndex = port;
  DWORD returned = 0;
  if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                       &data, sizeof(data), &data, sizeof(data), &returned, nullptr) ||
      returned < offsetof(USB_NODE_CONNECTION_INFORMATION_EX, PipeList) ||
      data.connection.ConnectionStatus != DeviceConnected || data.connection.DeviceIsHub ||
      data.connection.DeviceAddress == 0 || data.connection.CurrentConfigurationValue == 0)
    return std::nullopt;
  return PortState{data.connection.DeviceDescriptor, data.connection.DeviceAddress,
                   data.connection.CurrentConfigurationValue};
}

std::vector<unsigned char> Descriptor(HANDLE hub, ULONG port, UCHAR type, UCHAR index,
                                      USHORT language, USHORT length) {
  constexpr size_t start = offsetof(USB_DESCRIPTOR_REQUEST, Data);
  std::vector<unsigned char> bytes(start + length, 0);
  auto* request = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(bytes.data());
  request->ConnectionIndex = port;
  request->SetupPacket.wValue = (type << 8) | index;
  request->SetupPacket.wIndex = language;
  request->SetupPacket.wLength = length;
  DWORD returned = 0;
  if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, request, bytes.size(),
                       request, bytes.size(), &returned, nullptr) ||
      returned < start + 2 || returned > bytes.size() || bytes[start + 1] != type) return {};
  return {bytes.begin() + start, bytes.begin() + returned};
}

unsigned Read16(const std::vector<unsigned char>& bytes, size_t at) {
  return bytes[at] | (static_cast<unsigned>(bytes[at + 1]) << 8);
}

std::optional<StorageObservation> DescribeStorage(HANDLE hub, ULONG port, const PortState& state) {
  const auto& device = state.descriptor;
  if (device.bLength != 18 || device.bDescriptorType != USB_DEVICE_DESCRIPTOR_TYPE ||
      device.bNumConfigurations != 1) return std::nullopt;
  auto header = Descriptor(hub, port, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, 9);
  if (header.size() != 9 || header[0] != 9 || header[4] != 1 ||
      header[5] != state.configuration) return std::nullopt;
  unsigned length = Read16(header, 2);
  if (length < header.size()) return std::nullopt;
  auto config = Descriptor(hub, port, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, length);
  if (config.size() != length || !std::equal(header.begin(), header.end(), config.begin()))
    return std::nullopt;

  std::set<UCHAR> string_indexes{device.iManufacturer, device.iProduct, device.iSerialNumber,
                                config[6]};
  bool storage = false;
  for (size_t offset = 9; offset < config.size();) {
    const auto size = config[offset];
    if (size < 2 || size > config.size() - offset) return std::nullopt;
    if (config[offset + 1] == USB_INTERFACE_DESCRIPTOR_TYPE) {
      if (size < 9 || config[offset + 5] != 8) return std::nullopt;
      storage = true;
      string_indexes.insert(config[offset + 8]);
    }
    offset += size;
  }
  if (!storage) return std::nullopt;
  StorageObservation result;
  const auto* raw = reinterpret_cast<const unsigned char*>(&device);
  result.descriptors.assign(raw, raw + device.bLength);
  result.descriptors.insert(result.descriptors.end(), config.begin(), config.end());
  string_indexes.erase(0);
  if (string_indexes.empty()) return result;
  auto languages = Descriptor(hub, port, USB_STRING_DESCRIPTOR_TYPE, 0, 0, 255);
  if (languages.size() < 4 || languages[0] != languages.size() || languages[0] % 2)
    return result;
  for (UCHAR index : string_indexes) {
    auto text = Descriptor(hub, port, USB_STRING_DESCRIPTOR_TYPE, index, Read16(languages, 2), 255);
    if (text.empty() || text[0] != text.size() || text[0] % 2) continue;
    std::wstring decoded;
    for (size_t i = 2; i < text.size(); i += 2) decoded.push_back(Read16(text, i));
    result.strings.emplace(index, std::move(decoded));
  }
  return result;
}

bool SameDriver(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

std::optional<std::vector<std::wstring>> StorageInterfacePaths() {
  auto devices = Interfaces(kUsbDevice, true);
  if (!devices) return std::nullopt;
  std::vector<std::wstring> paths;
  for (auto& device : *devices) {
    const auto unique = std::count_if(devices->begin(), devices->end(), [&](const auto& other) {
      return SameDriver(device.driver, other.driver);
    });
    if (unique == 1) paths.push_back(std::move(device.path));
  }
  return paths;
}

std::optional<StorageObservation> ObserveStorage(const std::wstring& path) {
  auto devices = Interfaces(kUsbDevice, true);
  auto hubs = Interfaces(kUsbHub, false);
  if (!devices || !hubs) return std::nullopt;
  const auto device = std::find_if(devices->begin(), devices->end(), [&](const auto& candidate) {
    return SameDriver(path, candidate.path);
  });
  if (device == devices->end()) return std::nullopt;
  std::optional<StorageObservation> found;
  for (const auto& node : *hubs) {
    HANDLE handle = CreateFileW(node.path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) continue;
    const File hub(handle);
    USB_NODE_INFORMATION information{};
    information.NodeType = UsbHub;
    DWORD returned = 0;
    if (!DeviceIoControl(hub.get(), IOCTL_USB_GET_NODE_INFORMATION, &information,
                         sizeof(information), &information, sizeof(information), &returned,
                         nullptr) || returned < sizeof(information)) continue;
    for (ULONG port = 1; port <= information.u.HubInformation.HubDescriptor.bNumberOfPorts; ++port) {
      auto before = ConnectedPort(hub.get(), port);
      if (!before || !SameDriver(device->driver, PortDriver(hub.get(), port))) continue;
      if (found) return std::nullopt;
      auto observation = DescribeStorage(hub.get(), port, *before);
      auto after = ConnectedPort(hub.get(), port);
      if (!observation || !after || before->address != after->address ||
          before->configuration != after->configuration ||
          std::memcmp(&before->descriptor, &after->descriptor, sizeof(before->descriptor)) != 0 ||
          !SameDriver(device->driver, PortDriver(hub.get(), port))) return std::nullopt;
      found = std::move(observation);
    }
  }
  return found;
}

}  // namespace gesher::usb
