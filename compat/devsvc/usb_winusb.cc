// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "usb_winusb.h"

#include <cfgmgr32.h>
#include <objbase.h>
#include <setupapi.h>

#include <algorithm>
#include <cwchar>
#include <limits>
#include <map>
#include <mutex>

namespace gesher::usb {
namespace {

struct DestroyDeviceSet {
  void operator()(void* set) const { SetupDiDestroyDeviceInfoList(set); }
};

struct PathLess {
  bool operator()(const std::wstring& left, const std::wstring& right) const {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  }
};

struct Attachments {
  std::mutex lock;
  std::map<std::wstring, int64_t, PathLess> versions;
  int64_t generation = 0;
  HCMNOTIFICATION notification = nullptr;
  Attachments();
};

DWORD CALLBACK Changed(HCMNOTIFICATION, void* context, CM_NOTIFY_ACTION action,
                       PCM_NOTIFY_EVENT_DATA data, DWORD) {
  if (action != CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL &&
      action != CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) return ERROR_SUCCESS;
  auto* state = static_cast<Attachments*>(context);
  const wchar_t* path = data->u.DeviceInterface.SymbolicLink;
  std::lock_guard lock(state->lock);
  auto existing = state->versions.find(path);
  if (existing != state->versions.end()) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
      // Retire paths instead of accumulating every device ever attached. A
      // future observation creates a fresh monotonically increasing epoch.
      state->versions.erase(existing);
    } else {
      existing->second = state->generation == std::numeric_limits<int64_t>::max()
          ? -1 : ++state->generation;
    }
  }
  return ERROR_SUCCESS;
}

Attachments::Attachments() {
  CM_NOTIFY_FILTER filter{};
  filter.cbSize = sizeof(filter);
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
  if (CM_Register_Notification(&filter, this, Changed, &notification) != CR_SUCCESS)
    notification = nullptr;
}

Attachments& Monitor() {
  // The OS callback outlives dynamic teardown and is retired on process exit.
  static auto* state = new Attachments;
  return *state;
}

void ReadPaths(const GUID& interface_class, wchar_t* id, std::vector<std::wstring>* paths) {
  GUID guid = interface_class;
  constexpr ULONG flags = CM_GET_DEVICE_INTERFACE_LIST_PRESENT;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, &guid, id, flags) != CR_SUCCESS ||
        length < 2 || length > 32768) return;
    std::vector<wchar_t> interfaces(length);
    const CONFIGRET result = CM_Get_Device_Interface_ListW(
        &guid, id, interfaces.data(), length, flags);
    if (result == CR_BUFFER_SMALL) continue;
    if (result != CR_SUCCESS) return;
    for (const wchar_t* path = interfaces.data(); *path; path += std::wcslen(path) + 1) {
      paths->emplace_back(path);
    }
    return;
  }
}

std::vector<unsigned char> GetDescriptor(WINUSB_INTERFACE_HANDLE handle, UCHAR type,
                                         UCHAR index, USHORT language, ULONG length) {
  std::vector<unsigned char> bytes(length);
  ULONG actual = 0;
  if (!WinUsb_GetDescriptor(handle, type, index, language, bytes.data(), length, &actual) ||
      actual < 2 || actual > length || bytes[1] != type) return {};
  bytes.resize(actual);
  return bytes;
}

// Cancellation must be drained before an OVERLAPPED or its buffer can go out
// of scope, even if a completion races the timeout.
int Complete(HANDLE file, WINUSB_INTERFACE_HANDLE handle, OVERLAPPED* operation, BOOL started,
             DWORD timeout) {
  if (!started) {
    if (GetLastError() != ERROR_IO_PENDING) return -1;
    if (WaitForSingleObject(operation->hEvent, timeout == 0 ? INFINITE : timeout) != WAIT_OBJECT_0) {
      CancelIoEx(file, operation);
      ULONG discarded = 0;
      WinUsb_GetOverlappedResult(handle, operation, &discarded, TRUE);
      return -1;
    }
  }
  ULONG actual = 0;
  return WinUsb_GetOverlappedResult(handle, operation, &actual, TRUE)
      ? static_cast<int>(actual) : -1;
}

}  // namespace

bool AttachmentMonitoringAvailable() { return Monitor().notification != nullptr; }

int64_t AttachmentVersion(const wchar_t* path) {
  Attachments& state = Monitor();
  if (!state.notification) return -1;
  std::lock_guard lock(state.lock);
  auto existing = state.versions.find(path);
  if (existing != state.versions.end()) return existing->second;
  // Refuse another observation rather than silently dropping revocation
  // tracking. Removed entries are retired by the real PnP callback above.
  if (state.versions.size() >= 512 || state.generation == std::numeric_limits<int64_t>::max()) return -1;
  return state.versions.emplace(path, ++state.generation).first->second;
}

std::optional<std::vector<std::wstring>> InterfacePaths() {
  std::vector<std::wstring> paths;
  HDEVINFO raw = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (raw == INVALID_HANDLE_VALUE) return std::nullopt;
  const std::unique_ptr<void, DestroyDeviceSet> devices(raw);
  SP_DEVINFO_DATA info{};
  info.cbSize = sizeof(info);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(raw, i, &info); ++i) {
    wchar_t service[64]{};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(raw, &info, SPDRP_SERVICE, &type,
                                          reinterpret_cast<BYTE*>(service),
                                          sizeof(service) - sizeof(wchar_t), nullptr) ||
        type != REG_SZ || _wcsicmp(service, L"WinUSB") != 0) continue;
    wchar_t id[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(info.DevInst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) continue;

    HKEY key = SetupDiOpenDevRegKey(raw, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE) continue;
    wchar_t interfaces[1024]{};
    DWORD size = sizeof(interfaces) - 2 * sizeof(wchar_t);
    LSTATUS status = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type,
                                     reinterpret_cast<BYTE*>(interfaces), &size);
    if (status == ERROR_FILE_NOT_FOUND) {
      size = sizeof(interfaces) - 2 * sizeof(wchar_t);
      status = RegQueryValueExW(key, L"DeviceInterfaceGUID", nullptr, &type,
                               reinterpret_cast<BYTE*>(interfaces), &size);
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_MULTI_SZ)) continue;
    for (wchar_t* name = interfaces; *name != 0; name += std::wcslen(name) + 1) {
      GUID guid{};
      if (SUCCEEDED(CLSIDFromString(name, &guid))) ReadPaths(guid, id, &paths);
    }
  }
  if (GetLastError() != ERROR_NO_MORE_ITEMS) return std::nullopt;
  PathLess less;
  std::sort(paths.begin(), paths.end(), less);
  paths.erase(std::unique(paths.begin(), paths.end(), [&less](const auto& a, const auto& b) {
    return !less(a, b) && !less(b, a);
  }), paths.end());
  return paths;
}

File OpenDevice(const wchar_t* path, bool exclusive) {
  HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                            exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  return File(file == INVALID_HANDLE_VALUE ? nullptr : file);
}

Connection::Connection(HANDLE file) : file_(file) {
  if (!WinUsb_Initialize(file, &interface_)) { interface_ = nullptr; return; }
  UCHAR current = 0;
  if (WinUsb_GetCurrentAlternateSetting(interface_, &current)) {
    for (unsigned i = 0; i <= UCHAR_MAX; ++i) {
      if (!WinUsb_QueryInterfaceSettings(interface_, static_cast<UCHAR>(i), &descriptor_)) break;
      if (descriptor_.bAlternateSetting != current) continue;
      alternate_index_ = static_cast<UCHAR>(i);
      read_policies_set_ = ConfigureReadPipes();
      return;
    }
  }
  WinUsb_Free(interface_);
  interface_ = nullptr;
}

Connection::~Connection() {
  if (interface_ != nullptr) WinUsb_Free(interface_);
}

std::vector<unsigned char> Connection::Descriptors() const {
  if (!interface_) return {};
  auto bytes = GetDescriptor(interface_, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, 18);
  if (bytes.size() != 18 || bytes[0] != 18 || bytes[17] != 1) return {};
  const auto config = GetDescriptor(interface_, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, 9);
  if (config.size() != 9 || config[0] != 9 || config[4] != 1) return {};
  const ULONG length = config[2] | (static_cast<ULONG>(config[3]) << 8);
  if (length < config.size()) return {};
  const auto payload = GetDescriptor(interface_, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, length);
  if (payload.size() != length || !std::equal(config.begin(), config.end(), payload.begin()))
    return {};
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

std::wstring Connection::StringDescriptor(unsigned char index) const {
  if (!interface_ || index == 0) return {};
  auto languages = GetDescriptor(interface_, USB_STRING_DESCRIPTOR_TYPE, 0, 0, 255);
  if (languages.size() < 4 || languages[0] < 4 || languages[0] > languages.size()) return {};
  const USHORT language = static_cast<USHORT>(languages[2] | (static_cast<unsigned>(languages[3]) << 8));
  auto bytes = GetDescriptor(interface_, USB_STRING_DESCRIPTOR_TYPE, index, language, 255);
  if (bytes.empty() || bytes[0] < 2 || bytes[0] > bytes.size() || bytes[0] % 2 != 0) return {};
  std::wstring result;
  for (size_t i = 2; i < bytes[0]; i += 2)
    result.push_back(static_cast<wchar_t>(bytes[i] | (static_cast<unsigned>(bytes[i + 1]) << 8)));
  return result;
}

bool Connection::Claim(int number) {
  std::lock_guard lock(submission_);
  if (closed_ || !interface_ || number != descriptor_.bInterfaceNumber) return false;
  claimed_ = true;
  return true;
}

bool Connection::Release(int number) {
  std::lock_guard lock(submission_);
  if (closed_ || !claimed_ || number != descriptor_.bInterfaceNumber) return false;
  // Like usbfs, releasing an interface cancels its pending endpoint requests.
  // Keep requests alive until their canceled OVERLAPPED completions are reaped.
  bool released = true;
  for (UCHAR i = 0; i < descriptor_.bNumEndpoints; ++i) {
    WINUSB_PIPE_INFORMATION pipe{};
    if (!WinUsb_QueryPipe(interface_, alternate_index_, i, &pipe) ||
        !WinUsb_AbortPipe(interface_, pipe.PipeId)) released = false;
  }
  claimed_ = false;
  return released;
}

bool Connection::SetAlternate(int number, int alternate) {
  std::lock_guard lock(submission_);
  if (closed_ || !claimed_ || number != descriptor_.bInterfaceNumber ||
      alternate < 0 || alternate > 255)
    return false;
  USB_INTERFACE_DESCRIPTOR descriptor{};
  for (unsigned i = 0; i <= UCHAR_MAX; ++i) {
    if (!WinUsb_QueryInterfaceSettings(interface_, static_cast<UCHAR>(i), &descriptor)) return false;
    if (descriptor.bAlternateSetting != alternate) continue;
    if (!WinUsb_SetCurrentAlternateSetting(interface_, static_cast<UCHAR>(alternate))) return false;
    alternate_index_ = static_cast<UCHAR>(i);
    descriptor_ = descriptor;
    read_policies_set_ = ConfigureReadPipes();
    return true;
  }
  return false;
}

void Connection::Cancel() {
  std::lock_guard lock(submission_);
  closed_ = true;
  CancelIoEx(file_, nullptr);
}

bool Connection::FindPipe(int endpoint) const {
  if (!claimed_ || endpoint <= 0 || endpoint > 255) return false;
  for (UCHAR i = 0; i < descriptor_.bNumEndpoints; ++i) {
    WINUSB_PIPE_INFORMATION pipe{};
    if (!WinUsb_QueryPipe(interface_, alternate_index_, i, &pipe) ||
        pipe.PipeId != endpoint) continue;
    return pipe.PipeType == UsbdPipeTypeBulk || pipe.PipeType == UsbdPipeTypeInterrupt;
  }
  return false;
}

bool Connection::ConfigureReadPipes() {
  for (UCHAR i = 0; i < descriptor_.bNumEndpoints; ++i) {
    WINUSB_PIPE_INFORMATION pipe{};
    if (!WinUsb_QueryPipe(interface_, alternate_index_, i, &pipe)) return false;
    if ((pipe.PipeId & 0x80) == 0 ||
        (pipe.PipeType != UsbdPipeTypeBulk && pipe.PipeType != UsbdPipeTypeInterrupt)) continue;
    // An oversized USB response must fail this read. WinUSB's default would
    // instead return a partial transfer and feed the surplus into the next read.
    UCHAR allow_partial_reads = FALSE;
    if (!WinUsb_SetPipePolicy(interface_, pipe.PipeId, ALLOW_PARTIAL_READS,
                             sizeof(allow_partial_reads), &allow_partial_reads)) return false;
  }
  return true;
}

bool Connection::Submit(int endpoint, unsigned char* data, unsigned length, OVERLAPPED* op) {
  std::lock_guard lock(submission_);
  if (closed_ || length > INT_MAX || !FindPipe(endpoint) ||
      ((endpoint & 0x80) && !read_policies_set_)) return false;
  const BOOL result = endpoint & 0x80
      ? WinUsb_ReadPipe(interface_, static_cast<UCHAR>(endpoint), data, length, nullptr, op)
      : WinUsb_WritePipe(interface_, static_cast<UCHAR>(endpoint), data, length, nullptr, op);
  if (result) SetEvent(op->hEvent);
  return result || GetLastError() == ERROR_IO_PENDING;
}

int Connection::Result(OVERLAPPED* operation, bool wait) {
  ULONG actual = 0;
  return WinUsb_GetOverlappedResult(interface_, operation, &actual, wait)
      ? static_cast<int>(actual) : -1;
}

bool Connection::CancelRequest(OVERLAPPED* operation) {
  return CancelIoEx(file_, operation) != FALSE;
}

int Connection::Transfer(int endpoint, unsigned char* data, unsigned length, unsigned timeout) {
  File event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return -1;
  OVERLAPPED operation{};
  operation.hEvent = event.get();
  if (!Submit(endpoint, data, length, &operation)) return -1;
  if (WaitForSingleObject(event.get(), timeout == 0 ? INFINITE : timeout) == WAIT_OBJECT_0)
    return Result(&operation, false);
  CancelRequest(&operation);
  Result(&operation, true);
  return -1;
}

int Connection::Control(int type, int request, int value, int index, unsigned char* data,
                        unsigned length, unsigned timeout) {
  if (!interface_ || length > USHRT_MAX) return -1;
  if (type == 0x01 && request == USB_REQUEST_SET_INTERFACE && length == 0)
    return SetAlternate(index, value) ? 0 : -1;
  if (type == 0x00 && request == USB_REQUEST_SET_CONFIGURATION) return -1;
  File event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return -1;
  OVERLAPPED operation{};
  operation.hEvent = event.get();
  WINUSB_SETUP_PACKET setup{static_cast<UCHAR>(type), static_cast<UCHAR>(request),
                            static_cast<USHORT>(value), static_cast<USHORT>(index),
                            static_cast<USHORT>(length)};
  std::unique_lock lock(submission_);
  if (closed_) return -1;
  // WinUSB substitutes this interface number for interface-recipient requests.
  // Reject a different specified recipient before it can address this device.
  if ((setup.RequestType & 0x1f) == 1 &&
      static_cast<UCHAR>(setup.Index) != descriptor_.bInterfaceNumber) return -1;
  const BOOL result = WinUsb_ControlTransfer(interface_, setup, data, length, nullptr, &operation);
  const DWORD error = GetLastError();
  lock.unlock();
  SetLastError(error);
  return Complete(file_, interface_, &operation, result, timeout);
}

}  // namespace gesher::usb
