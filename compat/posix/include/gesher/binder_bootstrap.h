// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace gesher::binder_bootstrap {

enum class Endpoint : uint32_t { kBroker = 1, kSystemServer = 2, kClient = 3 };

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
  uint32_t version = 2;
  Endpoint endpoint{};
  uint64_t channel = 0;
  uint64_t broker = 0;
  uint64_t job_delivery = 0;
};
static_assert(sizeof(Startup) == 40);

inline bool ReadStartupRecord(Startup* result) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  GetStartupInfoW(&startup);
  if (startup.cbReserved2 != sizeof(Startup) || startup.lpReserved2 == nullptr) return false;
  std::memcpy(result, startup.lpReserved2, sizeof(*result));
  return result->crt_descriptors == 0 && result->magic == 0x474c4155 && result->version == 2;
}

inline bool ReadStartup(Endpoint endpoint, Startup* result) {
  Startup record;
  if (!ReadStartupRecord(&record) || record.endpoint != endpoint) return false;
  switch (endpoint) {
    case Endpoint::kBroker:
      if (GetFileType(DecodeHandle(record.channel)) != FILE_TYPE_PIPE ||
          record.broker != 0 || record.job_delivery != 0) return false;
      break;
    case Endpoint::kSystemServer:
      if (GetFileType(DecodeHandle(record.channel)) != FILE_TYPE_PIPE ||
          GetProcessId(DecodeHandle(record.broker)) == 0 ||
          GetFileType(DecodeHandle(record.job_delivery)) != FILE_TYPE_PIPE) return false;
      break;
    case Endpoint::kClient:
      if (record.channel != 0 || record.job_delivery != 0 ||
          GetProcessId(DecodeHandle(record.broker)) == 0) return false;
      break;
  }
  *result = record;
  return true;
}

}  // namespace gesher::binder_bootstrap
