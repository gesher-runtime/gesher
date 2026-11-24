// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include "audio_device_state.h"

#include <mutex>

#include "binder_bootstrap.h"
#include "windows_process_identity.h"

namespace gesher::record_locks { struct LinuxFlock; }
namespace gesher::rooted_storage_wire { struct Filesystem; }

namespace gesher::binder_endpoint {

constexpr DWORD kPinRights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

// Each protected child gets an actual process handle. Keeping it open avoids
// numeric PID reuse, and verifying every physical pipe protects all Binder and
// SCM_RIGHTS lanes when an endpoint becomes stale or unavailable.
class Binding {
 public:
  Binding() {
    binder_bootstrap::Startup startup;
    if (!binder_bootstrap::ReadStartupRecord(&startup) ||
        (startup.endpoint != binder_bootstrap::Endpoint::kClient &&
         startup.endpoint != binder_bootstrap::Endpoint::kApplication &&
         startup.endpoint != binder_bootstrap::Endpoint::kStandalone &&
         startup.endpoint != binder_bootstrap::Endpoint::kSystemServer)) return;
    required_ = true;
    guest_ = startup.endpoint == binder_bootstrap::Endpoint::kApplication ||
             startup.endpoint == binder_bootstrap::Endpoint::kStandalone;
    standalone_ = startup.endpoint == binder_bootstrap::Endpoint::kStandalone;
    system_ = startup.endpoint == binder_bootstrap::Endpoint::kSystemServer;
    HANDLE inherited = binder_bootstrap::DecodeHandle(startup.broker);
    if (GetProcessId(inherited) == 0) return;
    if (guest_ || startup.endpoint == binder_bootstrap::Endpoint::kClient) {
      if (startup.channel != 0 || startup.job_delivery != 0) return;
      broker_ = inherited;
      if (!SetHandleInformation(broker_, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(broker_);
        broker_ = nullptr;
      }
    } else {
      DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(),
                      &broker_, kPinRights, FALSE, 0);
    }
  }

  Binding(const Binding&) = delete;
  Binding& operator=(const Binding&) = delete;
  ~Binding() {
    if (broker_ != nullptr) CloseHandle(broker_);
  }

  bool PrepareOperator(DWORD pid, uint64_t birth) {
    process_identity::Process caller, peer;
    if (pid == 0 || birth == 0 ||
        !process_identity::ReadLiveProcess(GetCurrentProcess(), &caller) ||
        caller.principal.container.size != 0 || caller.principal.restricted ||
        caller.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID) return false;
    HANDLE process = OpenProcess(kPinRights, FALSE, pid);
    if (!process) return false;
    if (!process_identity::ReadLiveProcess(process, &peer) || peer.pid != pid || peer.birth != birth ||
        !(peer.principal == caller.principal)) {
      CloseHandle(process);
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (broker_ != nullptr) {
      const bool same = operator_ && GetProcessId(broker_) == pid &&
          WaitForSingleObject(broker_, 0) == WAIT_TIMEOUT;
      CloseHandle(process);
      return same;
    }
    if (required_) {
      CloseHandle(process);
      return false;
    }
    broker_ = process;
    required_ = operator_ = true;
    return true;
  }

  bool VerifyPipe(HANDLE pipe) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!required_) return true;
    ULONG owner = 0;
    return broker_ != nullptr && WaitForSingleObject(broker_, 0) == WAIT_TIMEOUT &&
           GetNamedPipeServerProcessId(pipe, &owner) && owner == GetProcessId(broker_) &&
           WaitForSingleObject(broker_, 0) == WAIT_TIMEOUT;
  }

  bool IsOperator() {
    std::lock_guard<std::mutex> lock(mutex_);
    return operator_;
  }

  bool IsGuest() const { return guest_; }
  bool IsStandalone() const { return standalone_; }
  bool UsesProtectedAudio() const { return guest_ || system_; }
  bool CanWriteAudioControls() const { return !guest_ || standalone_; }

  DWORD StandaloneRecordBrokerPid() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!standalone_ || !broker_ || WaitForSingleObject(broker_, 0) != WAIT_TIMEOUT) return 0;
    return GetProcessId(broker_);
  }

  bool DuplicateToGuestChild(HANDLE* duplicate) {
    std::lock_guard<std::mutex> lock(mutex_);
    return guest_ && broker_ != nullptr && WaitForSingleObject(broker_, 0) == WAIT_TIMEOUT &&
           DuplicateHandle(GetCurrentProcess(), broker_, GetCurrentProcess(),
                           duplicate, kPinRights, TRUE, 0);
  }

 private:
  std::mutex mutex_;
  HANDLE broker_ = nullptr;
  bool required_ = false;
  bool operator_ = false;
  bool guest_ = false;
  bool standalone_ = false;
  bool system_ = false;
};

inline Binding& Current() {
  static Binding binding;
  return binding;
}

bool EnrollChild(HANDLE process);
audio_state::Activity* OpenAudioPlaybackActivity();
bool IsAudioPlaybackActive(int stream, int in_past_ms);
int NewProtectedAudioSession();
audio_state::DeviceState* OpenAudioDeviceControls(bool writable);
HANDLE OpenAudioRender(const std::u16string& label);
HANDLE OpenAudioCapture(bool info_only);
HANDLE OpenCamera(bool info_only);
HANDLE OpenDropWorker();
int QueryAndroidDataCapacity(rooted_storage_wire::Filesystem* capacity);
int FileRecordLock(HANDLE file, int command, record_locks::LinuxFlock* input_and_output);
// Closes the caller-owned pin after the broker has captured file identity.
// The pointer is cleared only if the pin was closed.
int FileRecordClosed(HANDLE* pin);

}  // namespace gesher::binder_endpoint
