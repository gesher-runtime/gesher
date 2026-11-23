// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "peer_registry.h"
#include "../posix/include/gesher/binder_bootstrap.h"

namespace gesher::binder_launch {

using binder_bootstrap::Endpoint;
using binder_bootstrap::EncodeHandle;
using binder_bootstrap::DecodeHandle;
using binder_bootstrap::Startup;
using binder_bootstrap::ReadStartup;

enum class Action : uint32_t { kEnroll = 1, kHandoff, kRetire, kRevokeInstallation,
                               kRestoreInstallation };

struct Request {
  uint32_t magic = 0x474c4152;  // GLAR
  uint32_t version = 1;
  Action action{};
  binder_peer::Role role{};
  uint64_t process = 0;  // a handle duplicated into binderd
  uint64_t birth = 0;
  uint32_t pid = 0;
  uint32_t uid = 0;
  std::array<uint8_t, 16> installation{};
  process_identity::Sid container;
};

inline bool PrivatePipe(binder_peer::OwnedHandle* server, binder_peer::OwnedHandle* writer) {
  std::array<UCHAR, 24> random{};
  using Random = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
  const HMODULE bcrypt = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (bcrypt == nullptr) return false;
  const auto generate = reinterpret_cast<Random>(
      reinterpret_cast<void*>(GetProcAddress(bcrypt, "BCryptGenRandom")));
  const bool seeded = generate != nullptr &&
      BCRYPT_SUCCESS(generate(nullptr, random.data(), static_cast<ULONG>(random.size()),
                              BCRYPT_USE_SYSTEM_PREFERRED_RNG));
  FreeLibrary(bcrypt);
  if (!seeded) {
    return false;
  }
  std::wstring name = L"\\\\.\\pipe\\gesher-private-launch-";
  constexpr wchar_t kHex[] = L"0123456789abcdef";
  for (const auto byte : random) {
    name += kHex[byte >> 4];
    name += kHex[byte & 15];
  }
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GA;;;OW)S:(ML;;NW;;;ME)", SDDL_REVISION_1,
          &descriptor, nullptr)) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  binder_peer::OwnedHandle pipe(CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, &attributes));
  LocalFree(descriptor);
  if (pipe.get() == INVALID_HANDLE_VALUE) return false;
  binder_peer::OwnedHandle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
      nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (client.get() == INVALID_HANDLE_VALUE ||
      (!ConnectNamedPipe(pipe.get(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)) {
    return false;
  }
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(client.get(), &mode, nullptr, nullptr)) return false;
  *server = std::move(pipe);
  *writer = std::move(client);
  return true;
}

// Server endpoint blocks on its own control worker. The launcher endpoint uses
// PIPE_NOWAIT and a fixed deadline without retrying an ambiguous frame.
inline bool Transfer(HANDLE pipe, void* data, DWORD size, bool write, ULONGLONG deadline = 0) {
  auto* cursor = static_cast<BYTE*>(data);
  while (size != 0) {
    if (deadline != 0 && GetTickCount64() >= deadline) return false;
    DWORD moved = 0;
    const BOOL ok = write ? WriteFile(pipe, cursor, size, &moved, nullptr)
                          : ReadFile(pipe, cursor, size, &moved, nullptr);
    if (ok && moved != 0) {
      cursor += moved;
      size -= moved;
    } else if (deadline == 0 || (!ok && GetLastError() != ERROR_NO_DATA)) {
      return false;
    } else {
      Sleep(1);
    }
  }
  return true;
}

}  // namespace gesher::binder_launch
