// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>
#include <type_traits>

namespace gesher::usb_storage::test {

enum class ScmProfileControlOutcome : DWORD {
  not_attempted, inconclusive, delete_succeeded, delete_failed,
};

// This separate, fixed-size record never supplies the production test result.
// A failed or unavailable control is not evidence that a profile was unloaded.
struct ScmProfileControlEvidence {
  DWORD version = 1;
  DWORD attempted = 0;
  ScmProfileControlOutcome outcome = ScmProfileControlOutcome::not_attempted;
  DWORD status = ERROR_NOT_READY;
  DWORD identity_verified = 0, image_verified = 0, pid = 0;
  ULONGLONG process_creation = 0;
  DWORD profile_while_running = ERROR_NOT_READY, hive_while_running = ERROR_NOT_READY;
  DWORD retained_process_exit = 0, service_deleted = 0, handles_closed = 0;
  DWORD delete_attempts = 0, delete_status = ERROR_NOT_READY;
  DWORD profile_after_delete = ERROR_NOT_READY, hive_after_delete = ERROR_NOT_READY;
  DWORD cleanup_attempted = 0, cleanup_status = ERROR_NOT_READY, artifacts_retained = 0;
  ULONGLONG elapsed_ms = 0, remaining_worker_ms = 0;
  wchar_t service_name[80]{}, sid[96]{};
  char stage[64] = "not-attempted", cleanup_stage[64] = "not-attempted";
};
static_assert(std::is_trivially_copyable_v<ScmProfileControlEvidence> &&
              std::is_standard_layout_v<ScmProfileControlEvidence> &&
              sizeof(ScmProfileControlEvidence) <= 1024);

// Only the private test image implements this additional SCM entry point.
// Unknown modes are not consumed; malformed matching modes are handled errors.
bool DispatchScmProfileControlService(int argc, wchar_t** argv, DWORD* exit_code) noexcept;

// Call from the existing SYSTEM worker after primary cleanup, following an
// actual production failure or explicit fixture-only diagnostic opt-in.
// worker_deadline is the SAME absolute GetTickCount64
// deadline used by the controller's existing 90-second watchdog. This control
// also caps its own work at 15 seconds; it never extends the worker deadline.
// Storage is an exclusively created protected sibling of sealed_directory, so
// retained diagnostic artifacts cannot change the primary directory cleanup.
void ObserveScmProfileControl(const std::wstring& sealed_image,
    const std::wstring& sealed_directory, ULONGLONG worker_deadline,
    ScmProfileControlEvidence* evidence) noexcept;

// Use only the remaining portion of the caller's EXISTING cleanup deadline.
// Controller fallback requires its retained primary worker to have exited;
// the observing SYSTEM worker may also call this for its own scoped cleanup.
// A protected manifest binds cleanup to the owned service, image, SID and exact
// process. Missing ownership/lifetime proof retains objects and reports error.
// This result must never replace the production result or primary cleanup.
void CleanupScmProfileControl(const std::wstring& sealed_image,
    const std::wstring& sealed_directory, ULONGLONG existing_cleanup_deadline,
    ScmProfileControlEvidence* evidence) noexcept;

void PrintScmProfileControlEvidence(const ScmProfileControlEvidence& evidence) noexcept;

}  // namespace gesher::usb_storage::test
