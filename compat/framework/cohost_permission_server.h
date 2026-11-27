// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <objbase.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <gesher/cohost_permission_wire.h>
#include <gesher/windows_process_identity.h>

#include "../binder/peer_registry.h"
#include "app_container.h"

namespace gesher::cohost_permissions {

// One host-selected APK and exact native process generation. This name is
// published only in the read-only bootstrap of the process that owns it.
class Server {
 public:
  using Decisions = std::function<DWORD(Operation, const std::vector<std::u16string>&,
                                        std::vector<int32_t>*)>;
  using Clipboard = std::function<DWORD(Operation, const ClipboardFrame&, ClipboardFrame*)>;

  explicit Server(Decisions decide, Clipboard clipboard = {})
      : decide_(std::move(decide)), clipboard_(std::move(clipboard)) {
    GUID id;
    wchar_t suffix[40];
    if (SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, suffix, 40) != 0) {
      name_ = L"\\\\.\\pipe\\gesher-cohost-permissions-";
      name_ += suffix;
    }
  }

  ~Server() { Stop(); }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  const std::wstring& name() const { return name_; }

  bool IsLive() const {
    return !stopped_.load(std::memory_order_acquire) && child_ &&
        WaitForSingleObject(child_.get(), 0) == WAIT_TIMEOUT;
  }

  DWORD Start(HANDLE child, HANDLE job) {
    using process_identity::ReadLiveProcess;
    if (name_.empty() || !decide_ || child_) return ERROR_INVALID_PARAMETER;
    BOOL member = FALSE;
    if (!IsProcessInJob(child, job, &member) || !member || !ReadLiveProcess(child, &generation_) ||
        generation_.principal.container.size == 0 ||
        generation_.principal.integrity >= SECURITY_MANDATORY_MEDIUM_RID) return ERROR_ACCESS_DENIED;
    HANDLE retained = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), &retained,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) return GetLastError();
    child_.reset(retained);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto access = appcontainer::AppContainerPipeSecurity(&descriptor,
        generation_.principal.container.bytes.data());
    if (access != ERROR_SUCCESS) return access;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    DWORD error = ERROR_SUCCESS;
    for (size_t i = 0; i < instances_.size(); ++i) {
      auto& instance = instances_[i];
      instance.owner = this;
      const DWORD unique = i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0;
      instance.pipe.reset(CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | unique,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
          static_cast<DWORD>(instances_.size()), 65536, 65536, 0, &attributes));
      if (!instance.pipe) {
        error = GetLastError();
        break;
      }
      instance.thread.reset(CreateThread(nullptr, 0, RunInstance, &instance, 0, nullptr));
      if (!instance.thread) {
        error = GetLastError();
        break;
      }
    }
    LocalFree(descriptor);
    if (error != ERROR_SUCCESS) Stop();
    return error;
  }

 private:
  struct Instance {
    Server* owner = nullptr;
    binder_peer::OwnedHandle pipe;
    binder_peer::OwnedHandle thread;
  };

  static DWORD WINAPI RunInstance(void* context) {
    auto& instance = *static_cast<Instance*>(context);
    auto& owner = *instance.owner;
    HANDLE pipe = instance.pipe.get();
    while (owner.IsLive()) {
      if (!ConnectNamedPipe(pipe, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_LISTENING) { Sleep(2); continue; }
        if (error != ERROR_PIPE_CONNECTED) {
          if (error == ERROR_NO_DATA) { DisconnectNamedPipe(pipe); continue; }
          break;
        }
      }
      owner.Serve(pipe);
      DisconnectNamedPipe(pipe);
    }
    return 0;
  }

  void Serve(HANDLE pipe) {
    ULONG pid = 0;
    if (!IsLive() || !GetNamedPipeClientProcessId(pipe, &pid) || pid != generation_.pid) return;
    Header request;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    if (!binder_bootstrap::Transfer(pipe, &request, sizeof(request), false, deadline) ||
        !Valid(request)) return;
    if (IsClipboard(request.operation)) {
      if (clipboard_ && process_identity::MatchPipeSender(pipe, child_.get(), generation_)) {
        ServeClipboard(pipe, request);
      }
      return;
    }
    std::vector<std::u16string> names;
    if (!ReadNames(pipe, request.count, &names, deadline) ||
        !process_identity::MatchPipeSender(pipe, child_.get(), generation_)) return;
    std::vector<int32_t> result;
    DWORD error = decide_(request.operation, names, &result);
    if (!IsLive()) return;
    if (error == ERROR_SUCCESS && result.size() != names.size()) error = ERROR_INVALID_DATA;
    Reply reply;
    reply.error = static_cast<int32_t>(error);
    if (error == ERROR_SUCCESS) reply.count = static_cast<uint32_t>(result.size());
    const ULONGLONG send_deadline = GetTickCount64() + 5000;
    if (!binder_bootstrap::Transfer(pipe, &reply, sizeof(reply), true, send_deadline)) return;
    if (reply.count && !binder_bootstrap::Transfer(pipe, result.data(),
        reply.count * sizeof(int32_t), true, send_deadline)) return;
    AwaitClose(pipe, send_deadline);
  }

  void ServeClipboard(HANDLE pipe, const Header& request) {
    const ULONGLONG deadline = GetTickCount64() + kClipboardTransferTimeout;
    if (SendClipboardReply(pipe, request, deadline)) AwaitClose(pipe, deadline);
  }

  bool SendClipboardReply(HANDLE pipe, const Header& request, ULONGLONG deadline) {
    // At most one clipboard frame per lease can be buffered at a time. A
    // final pipe write can reach the caller before this thread is scheduled
    // to release the slot. Give the next request its bounded turn.
    std::unique_lock lock(clipboard_transfer_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(kClipboardTransferTimeout)) ||
        GetTickCount64() >= deadline || !IsLive()) return false;
    ClipboardFrame input, result;
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    if (input.Allocate(request.count)) {
      if (!TransferClipboard(pipe, input.data(), request.count, false, deadline) ||
          !IsLive()) return false;
      error = clipboard_(request.operation, input, &result);
    }
    if (!IsLive()) return false;
    if (error == ERROR_SUCCESS && !ValidClipboardReply(request.operation, result.size())) {
      error = ERROR_INVALID_DATA;
    }
    Reply reply;
    reply.error = static_cast<int32_t>(error);
    if (error == ERROR_SUCCESS) reply.count = static_cast<uint32_t>(result.size());
    return binder_bootstrap::Transfer(pipe, &reply, sizeof(reply), true, deadline) &&
        TransferClipboard(pipe, result.data(), reply.count, true, deadline);
  }

  void AwaitClose(HANDLE pipe, ULONGLONG send_deadline) {
    // DisconnectNamedPipe discards bytes the client has not read. An authorized
    // caller closes normally, and never holds the worker beyond this deadline.
    while (IsLive() && GetTickCount64() < send_deadline &&
           PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr)) Sleep(1);
  }

  void Stop() {
    stopped_.store(true, std::memory_order_release);
    // The decision callback must return when IsLive becomes false, including
    // when a still-visible host consent dialog is awaiting human input.
    for (auto& instance : instances_) {
      if (instance.thread) WaitForSingleObject(instance.thread.get(), INFINITE);
      instance.thread.reset();
      instance.pipe.reset();
    }
    child_.reset();
  }

  std::wstring name_;
  Decisions decide_;
  Clipboard clipboard_;
  std::timed_mutex clipboard_transfer_;
  std::atomic<bool> stopped_{false};
  binder_peer::OwnedHandle child_;
  process_identity::Process generation_;
  // One legitimate dialog may be outstanding while another Binder thread
  // checks permission state. The host request controller limits dialogs.
  std::array<Instance, 2> instances_;
};

}  // namespace gesher::cohost_permissions
