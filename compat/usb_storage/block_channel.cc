// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_channel.h"

#include <bcrypt.h>
#include <sddl.h>

#include <new>

namespace gesher::usb_storage {
namespace {

constexpr DWORD kPipeGuestAccess = FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE;
constexpr DWORD kGateGuestAccess = MUTEX_MODIFY_STATE | SYNCHRONIZE;

DWORD VerifyPending(HANDLE object, DWORD when_signaled) {
  const DWORD status = WaitForSingleObject(object, 0);
  if (status == WAIT_TIMEOUT) return ERROR_SUCCESS;
  if (status == WAIT_OBJECT_0) return when_signaled;
  return status == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE;
}

bool DuplicateTo(HANDLE source, HANDLE process, DWORD access, HANDLE* guest) {
  HANDLE copy = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), source, process, &copy, access, FALSE, 0)) {
    return false;
  }
  *guest = copy;
  return true;
}

void CloseProvisional(HANDLE process, HANDLE handle) {
  if (handle) {
    // DUPLICATE_CLOSE_SOURCE closes the remote copy even if copying fails.
    DuplicateHandle(process, handle, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
  }
}

}  // namespace

BlockChannel::~BlockChannel() {
  if (revocation_) Revoke();
  if (client_ != INVALID_HANDLE_VALUE) CloseHandle(client_);
  if (server_ != INVALID_HANDLE_VALUE) CloseHandle(server_);
  if (gate_) CloseHandle(gate_);
  if (revocation_) CloseHandle(revocation_);
}

DWORD BlockChannel::Revoke() {
  std::lock_guard lock(handoff_mutex_);
  return SetEvent(revocation_) ? ERROR_SUCCESS : GetLastError();
}

DWORD BlockChannel::Handoff(HANDLE process, BlockGuestHandles* result) {
  if (!process || process == INVALID_HANDLE_VALUE ||
      !result || result->pipe || result->gate || result->revoked) return ERROR_INVALID_PARAMETER;
  std::lock_guard lock(handoff_mutex_);
  if (client_ == INVALID_HANDLE_VALUE) return ERROR_ALREADY_ASSIGNED;
  DWORD status = VerifyPending(revocation_, ERROR_OPERATION_ABORTED);
  if (status != ERROR_SUCCESS) return status;
  status = VerifyPending(process, ERROR_PROCESS_ABORTED);
  if (status != ERROR_SUCCESS) return status;

  BlockGuestHandles guest;
  if (!DuplicateTo(client_, process, kPipeGuestAccess, &guest.pipe) ||
      !DuplicateTo(gate_, process, kGateGuestAccess, &guest.gate) ||
      !DuplicateTo(revocation_, process, SYNCHRONIZE, &guest.revoked)) {
    status = GetLastError();
  } else {
    status = VerifyPending(process, ERROR_PROCESS_ABORTED);
    if (status == ERROR_SUCCESS && !CloseHandle(client_)) status = GetLastError();
  }
  if (status != ERROR_SUCCESS) {
    CloseProvisional(process, guest.revoked);
    CloseProvisional(process, guest.gate);
    CloseProvisional(process, guest.pipe);
    return status;
  }
  client_ = INVALID_HANDLE_VALUE;
  *result = guest;
  return ERROR_SUCCESS;
}

DWORD BlockChannel::Create(std::unique_ptr<BlockChannel>* result) {
  if (!result || *result) return ERROR_INVALID_PARAMETER;
  std::unique_ptr<BlockChannel> channel(new (std::nothrow) BlockChannel);
  if (!channel) return ERROR_NOT_ENOUGH_MEMORY;
  // The creator retains its original handles. Seal the anonymous objects
  // against access upgrades through DuplicateHandle, including by an owner
  // that otherwise gets implicit WRITE_DAC. Narrowing the handles alone does
  // not prevent a token allowed by the object's DACL from widening them.
  SECURITY_ATTRIBUTES private_sync{sizeof(private_sync), nullptr, FALSE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;GA;;;OW)(D;;GA;;;WD)",
          SDDL_REVISION_1, &private_sync.lpSecurityDescriptor, nullptr)) return GetLastError();
  channel->gate_ = CreateMutexW(&private_sync, FALSE, nullptr);
  if (channel->gate_) channel->revocation_ = CreateEventW(&private_sync, TRUE, FALSE, nullptr);
  const DWORD sync_status = channel->revocation_ ? ERROR_SUCCESS : GetLastError();
  LocalFree(private_sync.lpSecurityDescriptor);
  if (sync_status != ERROR_SUCCESS) return sync_status;

  constexpr wchar_t prefix[] = L"\\\\.\\pipe\\gesher-usb-block-";
  constexpr wchar_t hex[] = L"0123456789abcdef";
  UCHAR nonce[16];
  if (BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
    return ERROR_GEN_FAILURE;
  }
  wchar_t name[sizeof(prefix) / sizeof(*prefix) + 2 * sizeof(nonce)]{};
  wchar_t* cursor = name;
  for (size_t i = 0; i + 1 < sizeof(prefix) / sizeof(*prefix); ++i) *cursor++ = prefix[i];
  for (const UCHAR byte : nonce) {
    *cursor++ = hex[byte >> 4];
    *cursor++ = hex[byte & 15];
  }
  channel->server_ = CreateNamedPipeW(name,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
      1, 65536, 65536, 0, nullptr);
  if (channel->server_ == INVALID_HANDLE_VALUE) return GetLastError();
  channel->client_ = CreateFileW(name, kPipeGuestAccess, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (channel->client_ == INVALID_HANDLE_VALUE) return GetLastError();

  ULONG client_pid = 0;
  if (!GetNamedPipeClientProcessId(channel->server_, &client_pid)) return GetLastError();
  if (client_pid != GetCurrentProcessId()) return ERROR_ACCESS_DENIED;
  *result = std::move(channel);
  return ERROR_SUCCESS;
}

}  // namespace gesher::usb_storage
