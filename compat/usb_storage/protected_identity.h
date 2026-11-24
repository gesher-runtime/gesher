// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace gesher::usb_storage {
inline constexpr wchar_t kRuntimeIdentityService[] = L"GesherProtectedRuntime";
inline constexpr wchar_t kRuntimeIdentityAccount[] = L"NT SERVICE\\GesherProtectedRuntime";

DWORD InstallRuntimeIdentityService(const std::wstring& executable);
DWORD UninstallRuntimeIdentityService(const std::wstring& executable);
int RunRuntimeIdentityService();
// Runs in a short-lived, authority-created desktop-user helper. This grants
// window presentation access only; it never grants access to protected data.
DWORD PrepareRuntimeDesktop(const wchar_t* runtime_sid, const wchar_t* capability_sid);

class ProtectedIdentity {
 public:
  static DWORD Create(const std::wstring& executable, DWORD desktop_session,
                      std::unique_ptr<ProtectedIdentity>* result);
  ~ProtectedIdentity();
  ProtectedIdentity(const ProtectedIdentity&) = delete;
  ProtectedIdentity& operator=(const ProtectedIdentity&) = delete;
  HANDLE token() const { return token_; }
  PSID sid() const { return const_cast<BYTE*>(sid_.data()); }
  DWORD session() const { return session_; }
 private:
  ProtectedIdentity() = default;
  HANDLE token_ = nullptr;
  HANDLE profile_ = nullptr;
  DWORD session_ = 0;
  std::vector<BYTE> sid_;
};
}  // namespace gesher::usb_storage
