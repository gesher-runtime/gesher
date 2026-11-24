// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "host_disk.h"

namespace gesher::usb_storage {
struct ProtectedSessionOptions {
  DWORD desktop_session = 0;
  std::wstring source_apk;
  std::wstring apk_sha256;
  std::wstring selected_interface;
  DWORD chooser_pid = 0;
  uint64_t chooser_start_time = 0;
  uint64_t selected_handle = 0;
  HostDiskIdentity selected_identity{};
  std::vector<uint64_t> selected_volume_handles;
  bool allow_write = false;
  std::wstring ready_event;
};

// Called only by the installed LocalSystem authority after validating its
// complete protected runtime. The caller keeps stop valid for the whole call.
DWORD RunProtectedAndroid(const std::wstring& directory, const std::wstring& executable,
                          const ProtectedSessionOptions& options, HANDLE stop);
// The explicit elevated launch entry point. The service ACL remains admin-only.
DWORD LaunchProtectedApk(const std::wstring& executable, const wchar_t* apk_path,
                         bool select_storage = true);
}  // namespace gesher::usb_storage
