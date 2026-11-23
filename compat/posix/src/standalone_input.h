// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <mutex>
#include <string>

#include <gesher/binder_bootstrap.h>

namespace gesher::standalone_input {

class Binding {
 public:
  Binding() {
    binder_bootstrap::Startup startup;
    // The input owns only these three handles. Binder may already have consumed
    // its separate startup handles by the time an app first opens a path.
    if (!binder_bootstrap::ReadStartupRecord(&startup) ||
        (startup.endpoint != binder_bootstrap::Endpoint::kStandalone &&
         startup.endpoint != binder_bootstrap::Endpoint::kApplication) ||
        startup.code_file == 0 || startup.code_channel == 0 || startup.code_name == 0) return;
    HANDLE file = binder_bootstrap::DecodeHandle(startup.code_file);
    HANDLE channel = binder_bootstrap::DecodeHandle(startup.code_channel);
    HANDLE name = binder_bootstrap::DecodeHandle(startup.code_name);
    using Name = binder_bootstrap::StandaloneName;
    auto* record = static_cast<const Name*>(MapViewOfFile(name, FILE_MAP_READ, 0, 0, sizeof(Name)));
    if (record != nullptr) {
      if (record->length > 0 && record->length < Name::kMaxPath &&
          record->path[record->length] == L'\0') {
        name_ = DosName(std::wstring(record->path, record->length));
      }
      UnmapViewOfFile(record);
    }
    CloseHandle(name);
    if (name_.empty() || GetFileType(file) != FILE_TYPE_DISK ||
        GetFileType(channel) != FILE_TYPE_PIPE ||
        !SetHandleInformation(file, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(channel, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(file);
      CloseHandle(channel);
      name_.clear();
      return;
    }
    file_ = file;
    channel_ = channel;
  }

  ~Binding() {
    if (channel_ != nullptr) CloseHandle(channel_);
    if (file_ != nullptr) CloseHandle(file_);
  }
  Binding(const Binding&) = delete;
  Binding& operator=(const Binding&) = delete;

  bool Matches(const std::wstring& candidate) const {
    if (name_.empty()) return false;
    std::wstring full(32768, L'\0');
    const DWORD size = GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(full.size()),
                                       full.data(), nullptr);
    if (size == 0 || size >= full.size()) return false;
    full.resize(size);
    full = DosName(std::move(full));
    return CompareStringOrdinal(name_.c_str(), -1, full.c_str(), -1, TRUE) == CSTR_EQUAL;
  }

  HANDLE Open(bool metadata_only, bool inheritable = false) {
    HANDLE result = nullptr;
    if (metadata_only) {
      if (DuplicateHandle(GetCurrentProcess(), file_, GetCurrentProcess(),
                          &result, 0, inheritable, DUPLICATE_SAME_ACCESS)) {
        return result;
      }
      return INVALID_HANDLE_VALUE;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (channel_ == nullptr) return INVALID_HANDLE_VALUE;
    uint32_t operation = binder_bootstrap::kOpenStandaloneInput;
    uint64_t received = 0;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    if (!binder_bootstrap::Transfer(channel_, &operation, sizeof(operation), true, deadline) ||
        !binder_bootstrap::Transfer(channel_, &received, sizeof(received), false, deadline)) {
      CloseHandle(channel_);
      channel_ = nullptr;
      return INVALID_HANDLE_VALUE;
    }
    result = binder_bootstrap::DecodeHandle(received);
    if (result == nullptr) return INVALID_HANDLE_VALUE;
    if (inheritable && !SetHandleInformation(result, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
      CloseHandle(result);
      return INVALID_HANDLE_VALUE;
    }
    return result;
  }

 private:
  static std::wstring DosName(std::wstring path) {
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + path.substr(8);
    if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    return path;
  }

  HANDLE file_ = nullptr;
  HANDLE channel_ = nullptr;
  std::wstring name_;
  std::mutex mutex_;
};

inline Binding& Current() {
  static Binding binding;
  return binding;
}

}  // namespace gesher::standalone_input
