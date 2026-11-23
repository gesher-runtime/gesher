// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gesher::binder_bootstrap {

enum class Endpoint : uint32_t { kBroker = 1, kSystemServer = 2, kClient = 3,
                                 kApplication = 4, kStandalone = 5, kRecordBroker = 6 };

inline uint64_t EncodeHandle(HANDLE handle) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

inline HANDLE DecodeHandle(uint64_t value) {
  return value <= UINTPTR_MAX ? reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)) : nullptr;
}

// Windows copies these bytes immediately after the zero CRT descriptor count.
// The locators name actual handles explicitly inherited into this process.
struct Startup {
  uint32_t crt_descriptors = 0;
  uint32_t magic = 0x474c4155;  // GLAU
  uint32_t version = 5;
  Endpoint endpoint{};
  uint64_t channel = 0;
  uint64_t broker = 0;
  uint64_t job_delivery = 0;
  uint64_t null_device = 0;
  uint32_t uid = 0;
  uint32_t reserved = 0;
  uint64_t code_file = 0;
  uint64_t code_channel = 0;
  uint64_t code_name = 0;
};
static_assert(sizeof(Startup) == 80);
inline constexpr uint32_t kOpenStandaloneInput = 0x47444f50;  // GDOP
struct StandaloneName {
  static constexpr uint32_t kMaxPath = 32768;
  uint32_t length;
  wchar_t path[kMaxPath];
};

inline bool ReadStartupRecord(Startup* result) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  GetStartupInfoW(&startup);
  if (startup.cbReserved2 < sizeof(Startup) || startup.lpReserved2 == nullptr) return false;
  std::memcpy(result, startup.lpReserved2 + startup.cbReserved2 - sizeof(*result), sizeof(*result));
  return result->crt_descriptors == 0 && result->magic == 0x474c4155 && result->version == 5 &&
         result->reserved == 0 &&
         (result->endpoint == Endpoint::kApplication || result->endpoint == Endpoint::kStandalone ||
          startup.cbReserved2 == sizeof(Startup));
}

inline bool ReadStartup(Endpoint endpoint, Startup* result) {
  Startup record;
  if (!ReadStartupRecord(&record) || record.endpoint != endpoint) return false;
  if (endpoint != Endpoint::kStandalone &&
      (record.code_channel != 0 || record.code_file != 0 || record.code_name != 0)) {
    return false;
  }
  switch (endpoint) {
    case Endpoint::kBroker:
    case Endpoint::kRecordBroker:
      if (GetFileType(DecodeHandle(record.channel)) != FILE_TYPE_PIPE ||
          record.broker != 0 || record.job_delivery != 0 || record.null_device != 0 ||
          record.uid != 0) return false;
      break;
    case Endpoint::kSystemServer:
      if (GetFileType(DecodeHandle(record.channel)) != FILE_TYPE_PIPE ||
          GetProcessId(DecodeHandle(record.broker)) == 0 ||
          GetFileType(DecodeHandle(record.job_delivery)) != FILE_TYPE_PIPE ||
          record.null_device != 0 || record.uid != 1000) return false;
      break;
    case Endpoint::kClient:
      if (record.channel != 0 || record.job_delivery != 0 ||
          GetProcessId(DecodeHandle(record.broker)) == 0 ||
          record.null_device != 0 || record.uid != 1000) return false;
      break;
    case Endpoint::kApplication:
      if (record.channel != 0 || record.job_delivery != 0 ||
          GetProcessId(DecodeHandle(record.broker)) == 0 ||
          GetFileType(DecodeHandle(record.null_device)) != FILE_TYPE_CHAR ||
          record.uid < 10000 || record.uid > 19999) return false;
      break;
    case Endpoint::kStandalone:
      if (record.channel != 0 || record.job_delivery != 0 ||
          GetProcessId(DecodeHandle(record.broker)) == 0 ||
          GetFileType(DecodeHandle(record.null_device)) != FILE_TYPE_CHAR ||
          record.uid < 10000 || record.uid > 19999 ||
          (record.code_file == 0) != (record.code_channel == 0) ||
          (record.code_file == 0) != (record.code_name == 0) ||
          (record.code_file != 0 && (GetFileType(DecodeHandle(record.code_file)) != FILE_TYPE_DISK ||
           GetFileType(DecodeHandle(record.code_channel)) != FILE_TYPE_PIPE))) return false;
      break;
  }
  *result = record;
  return true;
}

inline uint32_t GuestUid() {
  Startup record;
  if (!ReadStartupRecord(&record)) return 0;
  if ((record.endpoint == Endpoint::kApplication || record.endpoint == Endpoint::kStandalone) &&
      record.uid >= 10000 && record.uid <= 19999) {
    return record.uid;
  }
  if ((record.endpoint == Endpoint::kSystemServer || record.endpoint == Endpoint::kClient) &&
      record.uid == 1000) return 1000;
  return 0;
}

// A deadline is used only on a PIPE_NOWAIT endpoint. A partial frame ends the
// connection when its deadline expires.
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

}  // namespace gesher::binder_bootstrap
