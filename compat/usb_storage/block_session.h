// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "block_channel.h"
#include "host_disk.h"

namespace gesher::usb_storage {

// One explicit desktop choice and consent, pinned to the exact admitted APK
// process and media generation. Only the trusted authority constructs this
// object. Neither the selected path nor process handles come from an APK hello.
// An independent private channel is used for every authority-created child.
class BlockSession {
 public:
  // expires_at is an absolute GetTickCount64 deadline, chosen by the authority.
  // The authority must retain verified APK identity and obtain read/write
  // consent before calling. Open never takes a disk offline or changes media.
  static DWORD Open(const std::wstring& selected_interface, Access consent,
                    HANDLE admitted_parent, ULONGLONG expires_at,
                    std::unique_ptr<BlockSession>* result);
  // The service-retained selection, or the exact object imported from its
  // authenticated elevated chooser, remains the selected media after consent.
  static DWORD Adopt(std::unique_ptr<HostDisk> selected_disk, HANDLE admitted_parent,
                     ULONGLONG expires_at, std::unique_ptr<BlockSession>* result);
  ~BlockSession();
  BlockSession(const BlockSession&) = delete;
  BlockSession& operator=(const BlockSession&) = delete;

  // The caller authenticates a request from the retained parent and creates
  // the child suspended through AuthorityLaunch. It passes that creation
  // process object, never a PID reopened from a guest message. These handles
  // are valid only inside that child and are not inheritable.
  DWORD AttachChild(HANDLE suspended_child, BlockGuestHandles* result);
  DWORD Revoke();
  DWORD Validate();

 private:
  struct Child;
  BlockSession() = default;
  void Monitor();
  DWORD ValidateLocked();
  void RevokeLocked();

  HANDLE parent_ = nullptr;
  HANDLE stop_ = nullptr;
  ULONGLONG expires_at_ = 0;
  std::unique_ptr<HostDisk> disk_;
  std::vector<std::unique_ptr<Child>> children_;
  std::mutex mutex_;
  std::thread monitor_;
  DWORD terminal_ = ERROR_SUCCESS;
};

}  // namespace gesher::usb_storage
