// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_signal.h"

#include <new>

namespace gesher::usb_storage {

DWORD ProtectedSignalChannel::Create(HANDLE retained_child,
    std::unique_ptr<ProtectedSignalChannel>* result) try {
  if (!result || *result || !retained_child || retained_child == INVALID_HANDLE_VALUE ||
      GetProcessId(retained_child) == 0) return ERROR_INVALID_PARAMETER;
  std::unique_ptr<ProtectedSignalChannel> channel(new ProtectedSignalChannel);
  if (!DuplicateHandle(GetCurrentProcess(), retained_child, GetCurrentProcess(), &channel->child_,
      PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE |
      PROCESS_SUSPEND_RESUME | PROCESS_DUP_HANDLE, FALSE, 0)) return GetLastError();
  channel->section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      0, sizeof(ProtectedSignalMailbox), nullptr);
  if (!channel->section_) return GetLastError();
  channel->view_ = static_cast<ProtectedSignalMailbox*>(MapViewOfFile(channel->section_,
      FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ProtectedSignalMailbox)));
  if (!channel->view_) return GetLastError();
  channel->event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!channel->event_) return GetLastError();
  *result = std::move(channel);
  return ERROR_SUCCESS;
} catch (const std::bad_alloc&) {
  return ERROR_NOT_ENOUGH_MEMORY;
}

ProtectedSignalChannel::~ProtectedSignalChannel() {
  if (view_) UnmapViewOfFile(view_);
  if (section_) CloseHandle(section_);
  if (event_) CloseHandle(event_);
  if (child_) CloseHandle(child_);
}

DWORD ProtectedSignalChannel::Handoff(HANDLE* child_section, HANDLE* child_event) {
  if (!child_section || !child_event || *child_section || *child_event || handed_off_) {
    return ERROR_INVALID_PARAMETER;
  }
  HANDLE section = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), section_, child_, &section,
      FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0)) return GetLastError();
  HANDLE event = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), event_, child_, &event,
      SYNCHRONIZE, FALSE, 0)) {
    const DWORD error = GetLastError();
    DuplicateHandle(child_, section, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    return error;
  }
  *child_section = section;
  *child_event = event;
  handed_off_ = true;
  return ERROR_SUCCESS;
}

DWORD ProtectedSignalChannel::Send(int signal, DWORD sender_pid) {
  if (signal < 0 || signal > 64) return ERROR_INVALID_PARAMETER;
  const DWORD wait = WaitForSingleObject(child_, 0);
  if (wait != WAIT_TIMEOUT) return wait == WAIT_OBJECT_0 ? ERROR_PROCESS_ABORTED : GetLastError();
  if (signal == 0) return ERROR_SUCCESS;
  constexpr int kSigkill = 9, kSigstop = 19, kSigcont = 18;
  if (signal == kSigkill) return TerminateProcess(child_, 128 + signal) ? ERROR_SUCCESS : GetLastError();
  if (signal == kSigstop || signal == kSigcont) {
    using NtProcess = LONG(NTAPI*)(HANDLE);
    auto function = reinterpret_cast<NtProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
        signal == kSigstop ? "NtSuspendProcess" : "NtResumeProcess"));
    if (!function) return ERROR_NOT_SUPPORTED;
    if (function(child_) < 0) return ERROR_ACCESS_DENIED;
    if (signal == kSigstop) return ERROR_SUCCESS;
  }
  InterlockedExchange(&view_->senders[signal], static_cast<LONG>(sender_pid));
  InterlockedOr64(&view_->pending, static_cast<LONG64>(uint64_t{1} << (signal - 1)));
  return SetEvent(event_) ? ERROR_SUCCESS : GetLastError();
}

}  // namespace gesher::usb_storage
