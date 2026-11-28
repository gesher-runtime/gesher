// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <span>
#include <string_view>

namespace gesher::usb_storage {

// Identifies the last check reached by the unchanged namespace policy verifier.
// This is diagnostic output, never a policy selector or a reason to retry.
enum class AuthorityNamespacePolicyStage : DWORD {
  kNotAttempted, kSizeQuery, kQuery, kLength, kControl, kOwner, kGroup, kDacl, kLabel,
  kOwnerBounds, kGroupBounds, kDaclBounds, kLabelBounds, kDescriptor, kPolicy,
  kComplete,
};
struct AuthorityNamespacePolicyResult {
  AuthorityNamespacePolicyStage stage = AuthorityNamespacePolicyStage::kNotAttempted;
  LONG size_query_status = 0;
  DWORD allocation_bytes = 0;
  LONG query_status = 0;
  DWORD requested_information = 0;
  DWORD returned_bytes = 0, captured_bytes = 0;
  // A bounded prefix of the zero-initialized query allocation after NT success,
  // not a claim that this many bytes were returned. Some NPFS queries report
  // success with returned_bytes == 0; preserve the allocation bytes so that
  // an unwritten buffer can be distinguished from an unreported length.
  BYTE descriptor[320]{};
};

// Records only the checks the existing Recheck actually reached. An early
// failure leaves later policy snapshots unattempted; no diagnostic retry runs.
enum class AuthorityNamespaceRecheckStage : DWORD {
  kNotAttempted, kLeafOpen, kSamePin, kDirectoryPolicy, kPipePolicy, kComplete,
};
struct AuthorityNamespaceRecheckResult {
  AuthorityNamespaceRecheckStage stage = AuthorityNamespaceRecheckStage::kNotAttempted;
  DWORD attempts = 0;
  DWORD status = ERROR_NOT_READY;
  LONG leaf_open_status = 0;
  AuthorityNamespacePolicyResult directory_policy, pipe_policy;
};

// A single authority-only SACL finalization after the Windows lowbox wrapper.
// The pre-write full-SACL read and post-write strict read are separate receipts.
enum class AuthorityNamespaceSealStage : DWORD {
  kNotAttempted, kPrepared, kPrivilege, kBind, kOpen, kSamePin, kFullPolicy,
  kDescriptor, kWrite, kReadback, kClose, kRestore, kComplete,
};
struct AuthorityNamespaceSealResult {
  AuthorityNamespaceSealStage stage = AuthorityNamespaceSealStage::kNotAttempted;
  DWORD status = ERROR_NOT_READY;
  DWORD privilege_status = ERROR_NOT_READY, bind_status = ERROR_NOT_READY;
  DWORD privilege_before = 0, privilege_enabled = 0, privilege_restored = 0, privilege_changes = 0;
  DWORD privilege_restore_status = ERROR_NOT_READY, context_restore_status = ERROR_NOT_READY;
  LONG open_status = 0, write_status = 0;
  DWORD write_called = 0, close_called = 0, close_status = ERROR_NOT_READY;
  AuthorityNamespacePolicyResult before_policy, after_policy;
};

// Optional diagnostics for the authority's one package-directory creation.
// Values are written to trusted caller memory only; they never select behavior.
struct AuthorityNamespaceCreateResult {
  DWORD root_open_status = ERROR_NOT_READY;
  DWORD privilege_status = ERROR_NOT_READY;
  DWORD privilege_before = 0, privilege_enabled = 0, privilege_restored = 0;
  DWORD privilege_changes = 0, privilege_restore_status = ERROR_NOT_READY;
  DWORD bind_status = ERROR_NOT_READY;
  DWORD native_called = 0;
  LONG native_status = 0;
  DWORD pipe_open_status = ERROR_NOT_READY;
  DWORD pipe_create_called = 0;
  LONG pipe_create_status = 0;
  DWORD pipe_policy_status = ERROR_NOT_READY;
  AuthorityNamespacePolicyResult directory_policy, pipe_policy;
  AuthorityNamespaceRecheckResult recheck;
  AuthorityNamespaceSealResult reseal;
  DWORD context_restore_status = ERROR_NOT_READY;
};

struct AuthorityLaunchSpec {
  // Paths, arguments, complete Unicode environment and capabilities are supplied
  // by the trusted authority from its validated installation and session.
  const wchar_t* executable = nullptr;
  const wchar_t* directory = nullptr;
  std::wstring_view arguments;
  std::span<const wchar_t> environment;
  SECURITY_CAPABILITIES capabilities{};
  PSID desktop_user = nullptr;
  DWORD desktop_session = 0;
  // Optional trusted caller status, which must outlive the launch. This also
  // records cleanup of a suspended child if Create fails before enrollment.
  std::atomic<DWORD>* termination_error = nullptr;
  // Optional authority-owned job. Associate it atomically at process creation;
  // no job handle is inherited or supplied through a guest request.
  HANDLE job = nullptr;
  // Optional borrowed Directory from ProtectedIdentity. Pin the selected
  // session's object namespace across the private lowbox conversion. This is
  // authority-owned input, never a guest-supplied or inherited handle.
  HANDLE session_objects = nullptr;
  // The selected session's AppContainerNamedObjects root, under the same
  // authority-only ownership and lifetime rules. Retained access is read-only.
  HANDLE app_container_objects = nullptr;
  // Optional expected package Directory from an earlier protected launch.
  // A null value still requires complete protected leaf admission. Only the
  // all-null namespace path above retains the legacy desktop behavior.
  HANDLE expected_package_objects = nullptr;
  // Borrowed, optional output. Never a guest pointer, callback or policy input.
  AuthorityNamespaceCreateResult* namespace_create_result = nullptr;
};

class AuthorityLaunch {
 public:
  // The unimpersonated LocalSystem service supplies the desktop primary token
  // for its authenticated, selected session. No handles or environment are
  // inherited from the service. The process remains suspended on success.
  // The caller serializes same-package launches, including the Windows wrapper's
  // namespace update and the authority's subsequent policy finalization.
  static DWORD Create(HANDLE desktop_token, const AuthorityLaunchSpec& spec,
                      std::unique_ptr<AuthorityLaunch>* result,
                      const char** startup_stage = nullptr);
  ~AuthorityLaunch();
  AuthorityLaunch(const AuthorityLaunch&) = delete;
  AuthorityLaunch& operator=(const AuthorityLaunch&) = delete;

  // Borrowed authority-side process handle. Never grant it to a guest.
  HANDLE process() const { return guest_.hProcess; }
  DWORD pid() const { return guest_.dwProcessId; }
  DWORD main_thread_id() const { return guest_.dwThreadId; }
  // Borrowed non-inheritable read-only Directory; every launch owns its pin.
  HANDLE package_objects() const { return package_objects_; }
  // Retains the authenticated NPFS package directory through token conversion
  // and the child's lifetime. This handle is read-only and never inherited.
  HANDLE package_pipe_objects() const { return package_pipe_objects_; }

  // The authority serializes launch, handle handoff and resume. It retains the
  // launch object for the lifetime of the session. Destruction stops the guest.
  DWORD Resume();
  // Idempotent termination with an exact-process wait. Callers may share one
  // deadline across several children; destruction does not repeat that wait.
  DWORD Stop(DWORD timeout_milliseconds);

 private:
  AuthorityLaunch() = default;
  PROCESS_INFORMATION guest_{};
  HANDLE package_objects_ = nullptr;
  HANDLE package_pipe_objects_ = nullptr;
  bool resumed_ = false;
  bool stop_attempted_ = false;
  DWORD stop_status_ = ERROR_SUCCESS;
  std::atomic<DWORD>* termination_error_ = nullptr;
};

}  // namespace gesher::usb_storage
