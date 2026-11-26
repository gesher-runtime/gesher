// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <atomic>
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

// Kept by the authority beyond session destruction so cleanup never replaces
// an earlier startup error. These contain status codes, never token resources.
struct ProtectedIdentityCleanup {
  std::atomic<DWORD> profile_error{ERROR_SUCCESS};
  DWORD privilege_restore_error = ERROR_SUCCESS;
};

class ProtectedIdentity {
 public:
  static DWORD Create(const std::wstring& executable, DWORD desktop_session,
                      std::unique_ptr<ProtectedIdentity>* result,
                      const char** startup_stage,
                      ProtectedIdentityCleanup* cleanup);
  ~ProtectedIdentity();
  ProtectedIdentity(const ProtectedIdentity&) = delete;
  ProtectedIdentity& operator=(const ProtectedIdentity&) = delete;
  HANDLE token() const { return token_; }
  PSID sid() const { return const_cast<BYTE*>(sid_.data()); }
  DWORD session() const { return session_; }
  // Borrowed non-inheritable native Directory handle for the exact selected
  // session's BaseNamedObjects. Only query/traverse/read-control are retained;
  // this authority-side identity must outlive every launch that borrows it.
  HANDLE session_objects() const { return session_objects_; }
  // The corresponding AppContainerNamedObjects root is also retained with
  // query/traverse/read-control only. Creation rights stay in the verified
  // private account's access check, never in this borrowed handle.
  HANDLE app_container_objects() const { return app_container_objects_; }
  // Windows needs these per-user paths when creating a lowbox process. Derive
  // only the required values from this exact loaded profile, never the caller.
  DWORD AppendProfileEnvironment(std::vector<std::wstring>* environment) const;
  // Call after every process using the profile has stopped. A failed bounded
  // teardown is reported and must not be mistaken for a successful unload.
  DWORD UnloadProfile(DWORD users_stopped = ERROR_SUCCESS);
 private:
  explicit ProtectedIdentity(ProtectedIdentityCleanup* cleanup) : cleanup_(cleanup) {}
  HANDLE token_ = nullptr;
  HANDLE profile_ = nullptr;
  HANDLE session_objects_ = nullptr;
  HANDLE app_container_objects_ = nullptr;
  DWORD session_ = 0;
  ProtectedIdentityCleanup* cleanup_;
  bool unload_attempted_ = false;
  std::vector<BYTE> sid_;
};
}  // namespace gesher::usb_storage
