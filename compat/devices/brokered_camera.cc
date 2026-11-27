// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "brokered_camera.h"

#include <algorithm>
#ifndef GESHER_CAMERA_BROKER_HOST
#include <gesher/binder_endpoint.h>
#endif

namespace gesher::devices::camera {
namespace {

constexpr ULONGLONG kExchangeMs = 5000;
constexpr ULONGLONG kReleaseMs = 1500;
constexpr DWORD kMaxText = 8192;
constexpr uint32_t kMaxDevices = 64;
constexpr uint32_t kMaxModes = 512;
constexpr DWORD kMaxFrameBytes = 128 * 1024 * 1024;
constexpr uint8_t kRead = 1;
constexpr uint8_t kAck = 2;
constexpr uint8_t kFocus = 3;
constexpr uint8_t kClose = 4;

struct Geometry {
  int32_t width;
  int32_t height;
};

struct OpenReply {
  HRESULT result;
  Geometry size;
};

// Private success values keep the open reply and ACK byte-compatible with
// older peers. Only the camera pipe sees these values. A legacy S_OK carries no
// backend identity. Capture failures keep their original HRESULT unchanged.
constexpr HRESULT kOpenMfDevice = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x200);
constexpr HRESULT kOpenMfTest = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x201);
constexpr HRESULT kOpenSoftwareTest = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x202);

struct FocusReply {
  HRESULT result;
  long value;
  long flags;
};

DWORD Nv12Bytes(Geometry size) {
  if (size.width <= 0 || size.height <= 0 || size.width > 32768 || size.height > 32768 ||
      size.width % 2 || size.height % 2) return 0;
  const int64_t bytes = static_cast<int64_t>(size.width) * size.height * 3 / 2;
  return bytes <= kMaxFrameBytes ? static_cast<DWORD>(bytes) : 0;
}

void WaitForPipeProgress(DWORD milliseconds) {
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~Timer() { if (handle) CloseHandle(handle); }
  };
  thread_local Timer timer;
  LARGE_INTEGER due{};
  due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
  if (!timer.handle || !SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) ||
      WaitForSingleObject(timer.handle, INFINITE) != WAIT_OBJECT_0) {
    Sleep(milliseconds);
  }
}

// The camera reader and writer are both nonblocking. A coarse clock tick for
// each empty pipe buffer adds several ticks to a single video frame.
bool TransferBytes(HANDLE pipe, void* data, DWORD size, bool write, ULONGLONG deadline) {
  auto* bytes = static_cast<BYTE*>(data);
  while (size) {
    if (GetTickCount64() >= deadline) return false;
    DWORD moved = 0;
    const BOOL ok = write ? WriteFile(pipe, bytes, size, &moved, nullptr)
                          : ReadFile(pipe, bytes, size, &moved, nullptr);
    if (!ok && GetLastError() != ERROR_NO_DATA) return false;
    if (!moved) {
      WaitForPipeProgress(1);
      continue;
    }
    bytes += moved;
    size -= moved;
  }
  return true;
}

template <typename T>
bool Transfer(HANDLE pipe, T& value, bool write, ULONGLONG deadline) {
  return TransferBytes(pipe, &value, sizeof(value), write, deadline);
}

bool ExchangeText(HANDLE pipe, std::string& text, bool write, ULONGLONG deadline) {
  if (write && text.size() > kMaxText) return false;
  DWORD size = write ? static_cast<DWORD>(text.size()) : 0;
  if (!Transfer(pipe, size, write, deadline) || size > kMaxText) return false;
  if (!write) text.resize(size);
  return TransferBytes(pipe, text.data(), size, write, deadline);
}

bool Acknowledge(HANDLE pipe, bool write, ULONGLONG deadline) {
  uint8_t value = write ? kAck : 0;
  return Transfer(pipe, value, write, deadline) && value == kAck;
}

#ifndef GESHER_CAMERA_BROKER_HOST
bool ParseOpenBackend(HRESULT result, bool test_device, CaptureBackend* backend) {
  switch (result) {
    case S_OK:
      *backend = CaptureBackend::kUnreported;
      return true;
    case kOpenMfDevice:
      *backend = CaptureBackend::kMfDevice;
      return !test_device;
    case kOpenMfTest:
      *backend = CaptureBackend::kMfTestSource;
      return test_device;
    case kOpenSoftwareTest:
      *backend = CaptureBackend::kSoftwareTestSource;
      return test_device;
    default:
      return false;
  }
}

class CameraPipe {
 public:
  explicit CameraPipe(HANDLE handle) : handle_(handle) {}
  CameraPipe(const CameraPipe&) = delete;
  CameraPipe& operator=(const CameraPipe&) = delete;
  ~CameraPipe() {
    if (valid()) CloseHandle(handle_);
  }
  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return handle_; }
  HANDLE release() { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

 private:
  HANDLE handle_;
};

class BrokerCapture final : public Capture {
 public:
  BrokerCapture(HANDLE pipe, Geometry size, DWORD bytes, CaptureBackend source)
      : pipe_(pipe), bytes_(bytes) {
    width = size.width;
    height = size.height;
    backend = source;
  }

  ~BrokerCapture() override {
    if (failed_) return;
    // A subsequent open in this app can follow immediately. Wait for the
    // broker to retire the old capture, AppOps delivery, and session slot.
    const auto deadline = GetTickCount64() + kReleaseMs;
    uint8_t command = kClose;
    if (Transfer(pipe_.get(), command, true, deadline) &&
        Acknowledge(pipe_.get(), false, deadline)) {
      Acknowledge(pipe_.get(), true, deadline);
    }
  }

  HRESULT Read(uint8_t* frame) override {
    std::lock_guard lock(exchange_);
    if (failed_) return E_ABORT;
    const auto deadline = GetTickCount64() + kExchangeMs;
    uint8_t command = kRead;
    HRESULT result = E_ABORT;
    if (Transfer(pipe_.get(), command, true, deadline) &&
        Transfer(pipe_.get(), result, false, deadline)) {
      if (SUCCEEDED(result) && TransferBytes(
              pipe_.get(), frame, bytes_, false, deadline)) return result;
      if (FAILED(result)) {
        failed_ = true;
        Acknowledge(pipe_.get(), true, deadline);
        return result;
      }
    }
    failed_ = true;
    return E_ABORT;
  }

  HRESULT Focus(int op, long* value, long* flags) override {
    std::lock_guard lock(exchange_);
    *value = *flags = 0;
    if (failed_) return E_ABORT;
    const auto deadline = GetTickCount64() + kExchangeMs;
    uint8_t command = kFocus;
    int32_t operation = op;
    FocusReply reply{};
    if (!Transfer(pipe_.get(), command, true, deadline) ||
        !Transfer(pipe_.get(), operation, true, deadline) ||
        !Transfer(pipe_.get(), reply, false, deadline)) {
      failed_ = true;
      return E_ABORT;
    }
    if (SUCCEEDED(reply.result)) {
      *value = reply.value;
      *flags = reply.flags;
    }
    return reply.result;
  }

 private:
  CameraPipe pipe_;
  DWORD bytes_;
  std::mutex exchange_;
  bool failed_ = false;
};
#else
HRESULT OpenSuccess(CaptureBackend backend) {
  switch (backend) {
    case CaptureBackend::kMfDevice:
      return kOpenMfDevice;
    case CaptureBackend::kMfTestSource:
      return kOpenMfTest;
    case CaptureBackend::kSoftwareTestSource:
      return kOpenSoftwareTest;
    case CaptureBackend::kUnreported:
      return E_UNEXPECTED;
  }
  return E_UNEXPECTED;
}

constexpr ULONGLONG kAuthorizationProbeMs = 50;
constexpr DWORD kCommandPollMs = 5;

bool ServeList(HANDLE pipe, const std::function<bool()>& live) {
  std::string status;
  const auto devices = EnumerateBrokerDevices(&status);
  std::vector<const Device*> listed;
  for (const auto& device : devices) {
    if (device.id.size() <= kMaxText && device.name.size() <= kMaxText &&
        listed.size() < kMaxDevices) {
      listed.push_back(&device);
    }
  }
  const auto deadline = GetTickCount64() + kExchangeMs;
  auto count = static_cast<uint32_t>(listed.size());
  if (!live() || !ExchangeText(pipe, status, true, deadline) ||
      !Transfer(pipe, count, true, deadline)) return false;
  for (const Device* device : listed) {
    std::string id = device->id;
    std::string name = device->name;
    std::vector<Geometry> sizes;
    for (const auto& [width, height] : device->sizes) {
      if (Nv12Bytes({width, height}) && sizes.size() < kMaxModes) sizes.push_back({width, height});
    }
    auto modes = static_cast<uint32_t>(sizes.size());
    uint8_t autofocus = device->autofocus;
    if (!ExchangeText(pipe, id, true, deadline) || !ExchangeText(pipe, name, true, deadline) ||
        !Transfer(pipe, autofocus, true, deadline) ||
        !Transfer(pipe, modes, true, deadline) || !TransferBytes(
            pipe, sizes.data(), modes * sizeof(Geometry), true, deadline)) return false;
  }
  // DisconnectNamedPipe would discard bytes still buffered by the pipe.
  return Acknowledge(pipe, false, deadline);
}

bool AwaitCommand(HANDLE pipe, uint8_t* command, const std::function<bool()>& keep_capturing,
                  ULONGLONG deadline = 0) {
  ULONGLONG next_check = 0;
  for (;;) {
    const auto now = GetTickCount64();
    if (deadline && now >= deadline) return false;
    if (now >= next_check) {
      if (!keep_capturing()) return false;
      next_check = now + kAuthorizationProbeMs;
    }
    DWORD ready = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &ready, nullptr)) return false;
    if (ready) {
      return keep_capturing() &&
             Transfer(pipe, *command, false, now + kExchangeMs);
    }
    WaitForPipeProgress(kCommandPollMs);
  }
}

bool DeliverFrame(HANDLE pipe, uint8_t* frame, DWORD bytes, ULONGLONG deadline,
                  const std::function<bool()>& keep_capturing) {
  ULONGLONG next_check = 0;
  while (bytes) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    if (now >= next_check) {
      if (!keep_capturing()) return false;
      next_check = GetTickCount64() + kAuthorizationProbeMs;
    }
    DWORD moved = 0;
    if (!WriteFile(pipe, frame, std::min<DWORD>(bytes, 65536), &moved, nullptr) &&
        GetLastError() != ERROR_NO_DATA) return false;
    if (!moved) {
      WaitForPipeProgress(1);
      continue;
    }
    frame += moved;
    bytes -= moved;
  }
  return true;
}
#endif

}  // namespace

#ifndef GESHER_CAMERA_BROKER_HOST
static std::vector<Device> GetBrokeredDevices(std::string* status) {
  CameraPipe pipe(binder_endpoint::OpenCamera(true));
  *status = "The protected camera broker is unavailable";
  if (!pipe.valid()) return {};
  const auto deadline = GetTickCount64() + kExchangeMs;
  std::string description;
  uint32_t count = 0;
  if (!ExchangeText(pipe.get(), description, false, deadline) ||
      !Transfer(pipe.get(), count, false, deadline) || count > kMaxDevices) return {};
  std::vector<Device> devices;
  for (uint32_t i = 0; i < count; ++i) {
    Device device;
    uint32_t modes = 0;
    uint8_t autofocus = 0;
    if (!ExchangeText(pipe.get(), device.id, false, deadline) ||
        !ExchangeText(pipe.get(), device.name, false, deadline) || device.id.empty() ||
        !Transfer(pipe.get(), autofocus, false, deadline) || autofocus > 1 ||
        !Transfer(pipe.get(), modes, false, deadline) || modes > kMaxModes) return {};
    device.autofocus = autofocus != 0;
    device.test = device.id == kTestDeviceId;
    for (uint32_t j = 0; j < modes; ++j) {
      Geometry size{};
      if (!Transfer(pipe.get(), size, false, deadline) || !Nv12Bytes(size)) return {};
      device.sizes.emplace_back(size.width, size.height);
    }
    if (!device.sizes.empty()) devices.push_back(std::move(device));
  }
  if (!Acknowledge(pipe.get(), true, deadline)) return {};
  *status = std::move(description);
  return devices;
}

const std::vector<Device>& ListBrokered(std::string* status) {
  static std::string description;
  static const auto* devices = new std::vector<Device>(GetBrokeredDevices(&description));
  if (status) *status = description;
  return *devices;
}

std::unique_ptr<Capture> OpenBrokered(const std::string& id, int width, int height,
                                     capture_policy::Attribution attribution, HRESULT* hr) {
  *hr = E_ABORT;
  CameraPipe pipe(binder_endpoint::OpenCamera(false));
  if (!pipe.valid()) return nullptr;
  std::string symbol = id;
  Geometry size{width, height};
  OpenReply reply{};
  const auto deadline = GetTickCount64() + kExchangeMs;
  if (!Transfer(pipe.get(), size, true, deadline) ||
      !ExchangeText(pipe.get(), symbol, true, deadline) ||
      !Transfer(pipe.get(), attribution, true, deadline) ||
      !Transfer(pipe.get(), reply, false, deadline) ||
      !Acknowledge(pipe.get(), true, deadline)) return nullptr;
  *hr = reply.result;
  if (FAILED(*hr)) return nullptr;
  const DWORD bytes = Nv12Bytes(reply.size);
  CaptureBackend backend{};
  if (bytes == 0 || !ParseOpenBackend(reply.result, id == kTestDeviceId, &backend)) {
    *hr = E_UNEXPECTED;
    return nullptr;
  }
  *hr = S_OK;
  return std::make_unique<BrokerCapture>(pipe.release(), reply.size, bytes, backend);
}
#else
void ServeBrokeredCamera(HANDLE pipe, bool info_only, const std::function<bool()>& live,
                         const std::function<bool(const capture_policy::Attribution&)>& start,
                         const std::function<bool()>& authorized,
                         const std::function<bool()>& finish, bool slot) {
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) || !live()) return;
  if (info_only) {
    ServeList(pipe, live);
    return;
  }
  const auto deadline = GetTickCount64() + kExchangeMs;
  std::string id;
  Geometry requested{};
  capture_policy::Attribution attribution;
  if (!Transfer(pipe, requested, false, deadline) ||
      !ExchangeText(pipe, id, false, deadline) ||
      !Transfer(pipe, attribution, false, deadline) || !attribution.valid()) return;
  OpenReply reply{E_ACCESSDENIED, {}};
  std::unique_ptr<Capture> capture;
  const auto admitted = [&] {
    return live() && PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr) && authorized();
  };
  if (!slot) {
    reply.result = HRESULT_FROM_WIN32(ERROR_BUSY);
  } else if (live()) {
    reply.result = E_INVALIDARG;
    if (requested.width > 0 && requested.height > 0 &&
        requested.width <= 32768 && requested.height <= 32768 && !id.empty() &&
        id.find('\0') == std::string::npos && (id != kTestDeviceId || BrokerTestCameraEnabled())) {
      reply.result = E_ACCESSDENIED;
      if (start(attribution) && admitted()) {
        capture = OpenRevocable(id, requested.width, requested.height, &reply.result, admitted);
      }
    }
  }
  DWORD bytes = 0;
  if (capture) {
    reply.size = {capture->width, capture->height};
    bytes = Nv12Bytes(reply.size);
    if (!bytes) reply.result = E_UNEXPECTED;
    else if (!admitted()) reply.result = E_ACCESSDENIED;
    else if (SUCCEEDED(reply.result)) reply.result = OpenSuccess(capture->backend);
  }
  if (FAILED(reply.result)) capture.reset();
  if (!capture) finish();
  const auto handshake_deadline = GetTickCount64() + kExchangeMs;
  if (!Transfer(pipe, reply, true, handshake_deadline)) return;
  if (!capture) {
    Acknowledge(pipe, false, handshake_deadline);
    return;
  }
  uint8_t command = 0;
  if (!AwaitCommand(pipe, &command, admitted, handshake_deadline) || command != kAck) return;
  std::vector<uint8_t> frame(bytes);
  while (AwaitCommand(pipe, &command, admitted)) {
    if (command == kClose) {
      const auto release_deadline = GetTickCount64() + kReleaseMs;
      capture.reset();
      if (finish() && live() && Acknowledge(pipe, true, release_deadline)) {
        Acknowledge(pipe, false, release_deadline);
      }
      return;
    }
    if (command == kFocus) {
      const auto focus_deadline = GetTickCount64() + kExchangeMs;
      int32_t operation = -1;
      if (!Transfer(pipe, operation, false, focus_deadline) || !admitted()) return;
      FocusReply focus{E_INVALIDARG, 0, 0};
      if (operation >= kFocusGet && operation <= kFocusLock) {
        focus.result = capture->Focus(operation, &focus.value, &focus.flags);
      }
      if (!admitted() || !Transfer(pipe, focus, true, focus_deadline)) return;
      continue;
    }
    if (command != kRead) return;
    HRESULT result = capture->Read(frame.data());
    if (!admitted()) result = E_ACCESSDENIED;
    if (FAILED(result)) {
      capture.reset();
      finish();
    }
    const auto frame_deadline = GetTickCount64() + kExchangeMs;
    if (!Transfer(pipe, result, true, frame_deadline)) return;
    if (FAILED(result)) {
      Acknowledge(pipe, false, frame_deadline);
      return;
    }
    if (!DeliverFrame(pipe, frame.data(), bytes, frame_deadline, admitted)) return;
  }
}
#endif

}  // namespace gesher::devices::camera
