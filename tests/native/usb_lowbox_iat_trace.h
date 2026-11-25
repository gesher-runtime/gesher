// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <type_traits>

namespace gesher::usb_storage::test {

enum class LowboxTraceApi : DWORD {
  open_thread_token, open_thread_token_ex, open_process_token,
  open_process_token_ex, open_directory, create_directory,
  create_directory_ex, create_lowbox_token, create_symbolic_link,
  open_symbolic_link, query_security, set_security, duplicate_object,
  duplicate_token, query_token_information, set_token_information,
  status_to_win32, create_file, count,
};
inline constexpr DWORD kLowboxTraceSymbolCount = static_cast<DWORD>(LowboxTraceApi::count);
inline constexpr DWORD kLowboxTraceRecordCount = 64;

enum class LowboxSnapshotState : DWORD {
  not_observed, not_requested, absent, null_acl, complete, unreadable,
  invalid, truncated, incomplete,
};
template <SIZE_T Capacity>
struct LowboxTraceBytes {
  LowboxSnapshotState state = LowboxSnapshotState::not_observed;
  DWORD bytes = 0, copied = 0;
  BYTE data[Capacity]{};
};
struct LowboxDirectorySecurity {
  // Capture the first DirectoryEx input; count later calls without replacing
  // this evidence. These are input bytes, not the final object's descriptor.
  DWORD observations = 0, sequence = 0;
  LowboxSnapshotState state = LowboxSnapshotState::not_observed;
  DWORD descriptor_present = 0, revision = 0, control = 0;
  DWORD native_status = 0, returned_handle = 0;
  ULONG_PTR root = 0, shadow = 0, result = 0;
  LowboxTraceBytes<SECURITY_MAX_SID_SIZE> owner, group;
  LowboxTraceBytes<1024> dacl;
  LowboxTraceBytes<256> sacl;
};
struct LowboxSecurityQuery {
  // The most recent successful DACL-only query before the captured creation.
  // Correlation uses its sequence/source and the DirectoryEx shadow handle;
  // no object handle is retained and numeric equality is not lifetime proof.
  DWORD observations = 0, sequence = 0, information = 0;
  DWORD buffer_length = 0, returned_length = 0, native_status = 0;
  ULONG_PTR source = 0;
  LowboxTraceBytes<1536> descriptor;
};

struct LowboxTraceSymbolEvidence {
  DWORD imported = 0, patched = 0, restored = 0;
  DWORD status = ERROR_NOT_FOUND;
};
struct LowboxTraceCall {
  DWORD sequence = 0;
  LowboxTraceApi api = LowboxTraceApi::open_thread_token;
  DWORD access = 0, handle_attributes = 0, open_as_self = 0;
  DWORD object_attributes = 0, flags = 0, capability_count = 0, handle_count = 0;
  DWORD status = 0, returned_handle = 0, caller_rva = 0;
  DWORD input_snapshot_status = ERROR_SUCCESS;
  DWORD name_bytes = 0, name_characters = 0, name_truncated = 0;
  DWORD information_class = 0, buffer_length = 0, security_information = 0;
  DWORD token_type = 0, effective_only = 0, mapped_ntstatus = 0;
  DWORD target_snapshot_status = ERROR_SUCCESS;
  DWORD target_bytes = 0, target_characters = 0, target_truncated = 0;
  ULONG_PTR source = 0, root = 0, result = 0, shadow = 0;
  ULONG_PTR source_process = 0, target_process = 0;
  // Keep the total text budget bounded while retaining more final calls.
  // Original byte lengths and independent flags disclose either truncation;
  // shortened text never establishes an exact object or link-target identity.
  wchar_t name[96]{};
  wchar_t target[64]{};
};
struct LowboxIatTraceEvidence {
  DWORD version = 3;
  // Caller may set ONLY this metadata field before Begin. It is an expected
  // target session, not a claim that every recorded call had that token/session.
  DWORD expected_target_session = 0;
  DWORD attempted = 0, setup_status = ERROR_NOT_READY, restore_status = ERROR_NOT_READY;
  DWORD worker_thread = 0, worker_session = 0, worker_session_status = ERROR_NOT_READY;
  DWORD image_size = 0, slot_count = 0, page_count = 0;
  DWORD image_timestamp = 0, image_checksum = 0;
  DWORD missing_symbols = 0, uninspectable_imports = 0, delay_imports_present = 0;
  DWORD total_calls = 0, record_count = 0, overflow = 0;
  DWORD page_restore_status = ERROR_NOT_READY;
  LowboxTraceSymbolEvidence symbols[kLowboxTraceSymbolCount]{};
  LowboxDirectorySecurity directory_security;
  LowboxSecurityQuery security_query;
  // A bounded ring retains the last calls closest to the actual result.
  // sequence and overflow explicitly disclose overwritten earlier records.
  LowboxTraceCall records[kLowboxTraceRecordCount]{};
};
static_assert(std::is_trivially_copyable_v<LowboxIatTraceEvidence> &&
              std::is_standard_layout_v<LowboxIatTraceEvidence> &&
              sizeof(LowboxIatTraceEvidence) <= 65536);

// Explicit test opt-in only. This process permits one Begin, on the exact
// SYSTEM worker thread that will make the one real AuthorityLaunch::Create.
// Setup failures are separate observations; no launch retry is performed.
DWORD BeginLowboxIatTrace(LowboxIatTraceEvidence* evidence) noexcept;

// Always call End after an opted-in Begin, including failed setup/unwinding.
// It is idempotent for that same evidence record. All patched slots and page
// protections are restored before subsequent diagnostics or child Resume.
// Any nonzero restoration result must be a hard fixture cleanup failure;
// an earlier primary production failure remains the primary result.
DWORD EndLowboxIatTrace(LowboxIatTraceEvidence* evidence) noexcept;

// Call only after End. Does not claim coverage outside inspected ordinary
// KernelBase imports, or interpret an absent record as an absent native call.
void PrintLowboxIatTrace(const LowboxIatTraceEvidence& evidence) noexcept;

}  // namespace gesher::usb_storage::test
