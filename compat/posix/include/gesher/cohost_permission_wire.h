// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <gesher/cohost_bootstrap.h>

namespace gesher::cohost_permissions {

constexpr uint32_t kMagic = 0x47504353;  // GPCS
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxNames = 128;
constexpr uint32_t kMaxName = 300;
enum class Operation : uint32_t {
  kCheck = 1, kRequest = 2, kFlags = 3, kCanRequest = 4, kAppOpMode = 5
};

struct Header {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  Operation operation{};
  uint32_t count = 0;
};
static_assert(sizeof(Header) == 16);

struct Reply {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  int32_t error = 0;  // Win32 transport status; Android result values follow.
  uint32_t count = 0;
};
static_assert(sizeof(Reply) == 16);

inline bool Valid(const Header& head) {
  if (head.magic != kMagic || head.version != kVersion || head.count > kMaxNames) return false;
  switch (head.operation) {
    case Operation::kCheck:
    case Operation::kFlags:
    case Operation::kCanRequest:
    case Operation::kAppOpMode:
      return head.count == 1;
    case Operation::kRequest:
      return true;
  }
  return false;
}

inline bool ReadNames(HANDLE pipe, uint32_t count, std::vector<std::u16string>* names,
                      ULONGLONG deadline) {
  if (count > kMaxNames) return false;
  names->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t units = 0;
    if (!binder_bootstrap::Transfer(pipe, &units, sizeof(units), false, deadline) ||
        units > kMaxName) return false;
    std::u16string name(units, u'\0');
    if (units != 0 &&
        (!binder_bootstrap::Transfer(pipe, name.data(), units * sizeof(char16_t), false, deadline) ||
         name.find(u'\0') != std::u16string::npos)) return false;
    names->push_back(std::move(name));
  }
  return true;
}

inline bool SendRequest(HANDLE pipe, Operation operation, const std::vector<std::u16string>& names,
                        ULONGLONG deadline) {
  if (names.size() > kMaxNames) return false;
  Header request;
  request.operation = operation;
  request.count = static_cast<uint32_t>(names.size());
  if (!Valid(request)) return false;
  for (const auto& name : names) {
    if (name.size() > kMaxName || name.find(u'\0') != std::u16string::npos) return false;
  }
  if (!binder_bootstrap::Transfer(pipe, &request, sizeof(request), true, deadline)) return false;
  for (const auto& name : names) {
    uint32_t units = static_cast<uint32_t>(name.size());
    if (!binder_bootstrap::Transfer(pipe, &units, sizeof(units), true, deadline) ||
        (units != 0 && !binder_bootstrap::Transfer(pipe, const_cast<char16_t*>(name.data()),
            units * sizeof(char16_t), true, deadline))) return false;
  }
  return true;
}

// Each call opens its own endpoint. Permission queries need not wait for a
// human to answer a different request's real host consent dialog.
inline DWORD Exchange(Operation operation, const std::vector<std::u16string>& names,
                      std::vector<int32_t>* values) {
  const auto& bootstrap = cohost_bootstrap::Current();
  if (!bootstrap || bootstrap.permission_pipe.empty()) return ERROR_ACCESS_DENIED;
  const auto connect = [&] {
    return CreateFileW(bootstrap.permission_pipe.c_str(),
        FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  };
  HANDLE pipe = connect();
  if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
      WaitNamedPipeW(bootstrap.permission_pipe.c_str(), 5000)) pipe = connect();
  if (pipe == INVALID_HANDLE_VALUE) return GetLastError();
  const auto close = [&pipe](DWORD error) { CloseHandle(pipe); return error; };
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!bootstrap.VerifyPeer(pipe) || !SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) ||
      !SendRequest(pipe, operation, names, GetTickCount64() + 5000)) return close(ERROR_ACCESS_DENIED);
  Reply reply;
  const ULONGLONG query_timeout = GetTickCount64() + 5000;
  // Only the dedicated user-consent operation may wait for human input. Its
  // pipe is broken if the host authority terminates or retires the scope.
  const ULONGLONG deadline = operation == Operation::kRequest ? UINT64_MAX : query_timeout;
  if (!binder_bootstrap::Transfer(pipe, &reply, sizeof(reply), false, deadline) ||
      reply.magic != kMagic || reply.version != kVersion || !bootstrap.VerifyPeer(pipe)) {
    return close(ERROR_ACCESS_DENIED);
  }
  if (reply.error != 0) return close(static_cast<DWORD>(reply.error));
  if (reply.count != names.size()) return close(ERROR_INVALID_DATA);
  values->resize(reply.count);
  if (reply.count != 0 && !binder_bootstrap::Transfer(pipe, values->data(),
      reply.count * sizeof(int32_t), false, GetTickCount64() + 5000)) return close(ERROR_INVALID_DATA);
  return close(bootstrap.VerifyPeer(pipe) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
}

}  // namespace gesher::cohost_permissions
