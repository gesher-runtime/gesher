// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <gesher/binder_bootstrap.h>

namespace gesher::cohost_bootstrap {

// The host maps this record writable only while preparing it. Explicitly
// inherited section handles carry FILE_MAP_READ, and the separate host process
// handle pins the live native server generation without any control rights.
struct Metadata {
  uint32_t magic = 0x47434f48;  // GCOH
  uint32_t version = 2;
  std::array<wchar_t, binder_bootstrap::StandaloneName::kMaxPath> credential{};
  std::array<wchar_t, binder_bootstrap::StandaloneName::kMaxPath> device{};
  std::array<wchar_t, binder_bootstrap::StandaloneName::kMaxPath> credential_canonical{};
  std::array<wchar_t, binder_bootstrap::StandaloneName::kMaxPath> device_canonical{};
  std::array<wchar_t, binder_bootstrap::StandaloneName::kMaxPath> credential_nt{};
  std::array<wchar_t, binder_bootstrap::StandaloneName::kMaxPath> device_nt{};
  std::array<wchar_t, 256> storage_pipe{};
  std::array<wchar_t, 256> permission_pipe{};
};

template <size_t Size>
inline bool Store(std::array<wchar_t, Size>* field, std::wstring_view name) {
  if (name.size() >= Size || name.find(L'\0') != std::wstring_view::npos) return false;
  std::copy(name.begin(), name.end(), field->begin());
  (*field)[name.size()] = L'\0';
  return true;
}

template <size_t Size>
inline bool Load(const std::array<wchar_t, Size>& field, std::wstring* value) {
  const auto end = std::find(field.cbegin(), field.cend(), L'\0');
  if (end == field.cend()) return false;
  value->assign(field.cbegin(), end);
  return true;
}

class Binding {
 public:
  Binding() {
    binder_bootstrap::Startup startup;
    if (!binder_bootstrap::ReadStartupRecord(&startup) ||
        startup.endpoint != binder_bootstrap::Endpoint::kStandalone ||
        startup.cohost_section == 0 || startup.cohost_host == 0) return;
    HANDLE token = nullptr;
    DWORD lowbox = 0, size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    const bool contained = GetTokenInformation(token, TokenIsAppContainer,
        &lowbox, sizeof(lowbox), &size) && lowbox != 0;
    CloseHandle(token);
    if (!contained) return;
    section_ = binder_bootstrap::DecodeHandle(startup.cohost_section);
    storage_host = binder_bootstrap::DecodeHandle(startup.cohost_host);
    if (!section_ || !storage_host ||
        !SetHandleInformation(section_, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(storage_host, HANDLE_FLAG_INHERIT, 0) ||
        GetProcessId(storage_host) == 0) return;
    const auto* metadata = static_cast<const Metadata*>(
        MapViewOfFile(section_, FILE_MAP_READ, 0, 0, sizeof(Metadata)));
    if (metadata == nullptr) return;
    valid_ = metadata->magic == 0x47434f48 && metadata->version == 2 &&
        Load(metadata->credential, &credential) && Load(metadata->device, &device) &&
        Load(metadata->credential_canonical, &credential_canonical) &&
        Load(metadata->device_canonical, &device_canonical) &&
        Load(metadata->credential_nt, &credential_nt) && Load(metadata->device_nt, &device_nt) &&
        Load(metadata->storage_pipe, &storage_pipe) && Load(metadata->permission_pipe, &permission_pipe) &&
        ((!storage_pipe.empty() && !credential.empty() && !device.empty()) ||
         (storage_pipe.empty() && credential.empty() && device.empty())) &&
        (!storage_pipe.empty() || !permission_pipe.empty());
    UnmapViewOfFile(metadata);
  }

  ~Binding() {
    if (section_) CloseHandle(section_);
    if (storage_host) CloseHandle(storage_host);
  }

  Binding(const Binding&) = delete;
  Binding& operator=(const Binding&) = delete;

  explicit operator bool() const { return valid_; }

  bool VerifyPeer(HANDLE client_pipe) const {
    ULONG pid = 0;
    return valid_ && WaitForSingleObject(storage_host, 0) == WAIT_TIMEOUT &&
        GetNamedPipeServerProcessId(client_pipe, &pid) && pid == GetProcessId(storage_host) &&
        WaitForSingleObject(storage_host, 0) == WAIT_TIMEOUT;
  }

  bool DuplicateForChild(HANDLE* mapping, HANDLE* host) const {
    if (!valid_ || !DuplicateHandle(GetCurrentProcess(), section_, GetCurrentProcess(),
                                    mapping, 0, TRUE, DUPLICATE_SAME_ACCESS)) return false;
    if (DuplicateHandle(GetCurrentProcess(), storage_host, GetCurrentProcess(),
                        host, 0, TRUE, DUPLICATE_SAME_ACCESS)) return true;
    CloseHandle(*mapping);
    *mapping = nullptr;
    return false;
  }

  std::wstring credential;
  std::wstring device;
  std::wstring credential_canonical;
  std::wstring device_canonical;
  std::wstring credential_nt;
  std::wstring device_nt;
  std::wstring storage_pipe;
  std::wstring permission_pipe;
  HANDLE storage_host = nullptr;

 private:
  HANDLE section_ = nullptr;
  bool valid_ = false;
};

inline const Binding& Current() {
  static const Binding state;
  return state;
}

}  // namespace gesher::cohost_bootstrap
