// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_session.h"

#include "block_broker_server.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <system_error>

namespace gesher::usb_storage {
namespace {

DWORD RetainProcess(HANDLE process, HANDLE* retained) {
  if (!process || process == INVALID_HANDLE_VALUE || GetProcessId(process) == 0) {
    return ERROR_INVALID_HANDLE;
  }
  if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), retained,
                       SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
    return GetLastError();
  }
  const DWORD state = WaitForSingleObject(*retained, 0);
  if (state == WAIT_TIMEOUT) return ERROR_SUCCESS;
  const DWORD error = state == WAIT_OBJECT_0 ? ERROR_PROCESS_ABORTED : GetLastError();
  CloseHandle(*retained);
  *retained = nullptr;
  return error;
}

}  // namespace

struct BlockSession::Child {
  HANDLE process = nullptr;
  std::unique_ptr<BlockChannel> channel;
  std::thread server;
  std::atomic<bool> complete{false};

  ~Child() {
    if (channel) channel->Revoke();
    if (server.joinable()) server.join();
    if (process) CloseHandle(process);
  }
};

DWORD BlockSession::Open(const std::wstring& selected_interface, Access consent,
                         HANDLE admitted_parent, ULONGLONG expires_at,
                         std::unique_ptr<BlockSession>* result) try {
  if (!result || *result || expires_at <= GetTickCount64() ||
      (consent != Access::kReadOnly && consent != Access::kOfflineReadWrite)) {
    return ERROR_INVALID_PARAMETER;
  }
  std::unique_ptr<HostDisk> disk;
  const DWORD opened = HostDisk::Open(selected_interface, consent, &disk);
  if (opened != ERROR_SUCCESS) return opened;
  return Adopt(std::move(disk), admitted_parent, expires_at, result);
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
} catch (const std::system_error&) {
  return ERROR_NOT_READY;
}

DWORD BlockSession::Adopt(std::unique_ptr<HostDisk> selected_disk, HANDLE admitted_parent,
                          ULONGLONG expires_at, std::unique_ptr<BlockSession>* result) try {
  if (!selected_disk || !result || *result || expires_at <= GetTickCount64()) return ERROR_INVALID_PARAMETER;
  std::unique_ptr<BlockSession> session(new BlockSession);
  DWORD error = RetainProcess(admitted_parent, &session->parent_);
  if (error != ERROR_SUCCESS) return error;
  session->expires_at_ = expires_at;
  session->stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!session->stop_) return GetLastError();
  session->disk_ = std::move(selected_disk);
  error = session->Validate();
  if (error != ERROR_SUCCESS) return error;
  session->monitor_ = std::thread([owned = session.get()] { owned->Monitor(); });
  *result = std::move(session);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
} catch (const std::system_error&) {
  return ERROR_NOT_READY;
}

BlockSession::~BlockSession() {
  Revoke();
  if (monitor_.joinable()) monitor_.join();
  // Keep the selected disk alive until every in-flight operation has returned.
  children_.clear();
  if (stop_) CloseHandle(stop_);
  if (parent_) CloseHandle(parent_);
}

void BlockSession::RevokeLocked() {
  if (stop_) SetEvent(stop_);
  for (const auto& child : children_) child->channel->Revoke();
}

DWORD BlockSession::Revoke() {
  std::lock_guard lock(mutex_);
  if (terminal_ == ERROR_SUCCESS) terminal_ = ERROR_OPERATION_ABORTED;
  RevokeLocked();
  return ERROR_SUCCESS;
}

DWORD BlockSession::ValidateLocked() {
  if (terminal_ != ERROR_SUCCESS) return terminal_;
  if (!parent_ || !disk_ || !stop_) return ERROR_NOT_READY;
  const DWORD state = WaitForSingleObject(parent_, 0);
  if (state != WAIT_TIMEOUT) {
    terminal_ = state == WAIT_OBJECT_0 ? ERROR_PROCESS_ABORTED : GetLastError();
  } else if (GetTickCount64() >= expires_at_) {
    terminal_ = ERROR_TIMEOUT;
  } else {
    terminal_ = disk_->Validate();
  }
  if (terminal_ != ERROR_SUCCESS) RevokeLocked();
  return terminal_;
}

DWORD BlockSession::Validate() {
  std::lock_guard lock(mutex_);
  return ValidateLocked();
}

DWORD BlockSession::AttachChild(HANDLE suspended_child, BlockGuestHandles* result) try {
  if (!result || result->pipe || result->gate || result->revoked) return ERROR_INVALID_PARAMETER;
  std::unique_lock lock(mutex_);
  DWORD error = ValidateLocked();
  if (error != ERROR_SUCCESS) return error;
  // Finished children cannot accumulate during a long sequence of shell tools.
  std::erase_if(children_, [](const auto& child) { return child->complete.load(); });
  constexpr size_t kMaximumChildren = 64;
  if (children_.size() >= kMaximumChildren) return ERROR_NOT_ENOUGH_QUOTA;
  auto child = std::make_unique<Child>();
  error = RetainProcess(suspended_child, &child->process);
  if (error != ERROR_SUCCESS) return error;
  error = BlockChannel::Create(&child->channel);
  if (error != ERROR_SUCCESS) return error;
  // Reserve before publishing any remote handle. After successful handoff no
  // allocation may leave an untracked grant behind.
  children_.reserve(children_.size() + 1);
  Child* state = child.get();
  state->server = std::thread([this, state, disk = disk_.get()] {
    ServeBlockRequests(*state->channel, *disk, [this] { return Validate(); });
    state->complete.store(true);
  });
  error = child->channel->Handoff(suspended_child, result);
  if (error != ERROR_SUCCESS) {
    // A provisional remote endpoint may already have caused the worker to
    // enter Validate(). Do not join it while holding its authorization lock.
    child->channel->Revoke();
    lock.unlock();
    child.reset();
    return error;
  }
  children_.push_back(std::move(child));
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
} catch (const std::system_error&) {
  return ERROR_NOT_READY;
}

void BlockSession::Monitor() {
  const HANDLE waits[] = {stop_, parent_};
  for (;;) {
    const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 100);
    std::lock_guard lock(mutex_);
    if (waited == WAIT_OBJECT_0 || terminal_ != ERROR_SUCCESS) return;
    if (waited == WAIT_FAILED) {
      terminal_ = GetLastError();
      RevokeLocked();
      return;
    }
    if (ValidateLocked() != ERROR_SUCCESS) return;
    for (const auto& child : children_) {
      // A shell exiting cannot leave its data endpoint alive in another process.
      if (WaitForSingleObject(child->process, 0) != WAIT_TIMEOUT) child->channel->Revoke();
    }
  }
}

}  // namespace gesher::usb_storage
