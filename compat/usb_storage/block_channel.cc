// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_channel.h"

#include <bcrypt.h>

#include <new>

namespace gesher::usb_storage {

BlockChannel::~BlockChannel() {
  if (shutdown) SetEvent(shutdown);
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
  if (gate) CloseHandle(gate);
  if (shutdown) CloseHandle(shutdown);
}

DWORD BlockChannel::Create(std::unique_ptr<BlockChannel>* result) {
  if (!result || *result) return ERROR_INVALID_PARAMETER;
  std::unique_ptr<BlockChannel> channel(new (std::nothrow) BlockChannel);
  if (!channel) return ERROR_NOT_ENOUGH_MEMORY;
  channel->gate = CreateMutexW(nullptr, FALSE, nullptr);
  if (!channel->gate) return GetLastError();
  channel->shutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!channel->shutdown) return GetLastError();

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
  channel->server = CreateNamedPipeW(name,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
      1, 65536, 65536, 0, nullptr);
  if (channel->server == INVALID_HANDLE_VALUE) return GetLastError();
  channel->client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (channel->client == INVALID_HANDLE_VALUE) return GetLastError();

  ULONG client_pid = 0;
  if (!GetNamedPipeClientProcessId(channel->server, &client_pid)) return GetLastError();
  if (client_pid != GetCurrentProcessId()) return ERROR_ACCESS_DENIED;
  *result = std::move(channel);
  return ERROR_SUCCESS;
}

}  // namespace gesher::usb_storage
