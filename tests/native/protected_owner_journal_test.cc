// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Real Windows file locking and durable records, with disposable OUT state.
#include "../../compat/usb_storage/protected_owner_journal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace gesher::usb_storage;
namespace {
void Check(bool ok, const char *label) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s Win32=%lu\n", label, GetLastError());
    std::exit(1);
  }
}
ProtectedOwnerRecord Record(const wchar_t *owner, wchar_t signer) {
  ProtectedOwnerRecord r{};
  std::copy_n(owner, 36, r.installation);
  std::fill_n(r.signer, 64, signer);
  return r;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  Check(argc == 2, "explicit disposable output directory");
  Check(CreateDirectoryW(argv[1], nullptr) ||
            GetLastError() == ERROR_ALREADY_EXISTS,
        "fixture state directory");
  const auto old = Record(L"11111111-2222-3333-4444-555555555555", L'a');
  const auto next = Record(L"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", L'a');
  {
    ProtectedOwnerJournal journal;
    Check(journal.Open(argv[1]) == 0, "own journal opens");
    ProtectedOwnerJournal concurrent;
    Check(concurrent.Open(argv[1]) == ERROR_SHARING_VIOLATION,
          "actual concurrent writer excluded");
    ProtectedOwnerRecord value{};
    bool exists = true;
    Check(journal.Current(&value, &exists) == 0 && !exists,
          "first admission has no invented history");
    Check(journal.Publish(old) == 0, "first verified owner published");
    std::wstring current_path(32768, L'\0');
    const DWORD length =
        GetFullPathNameW(argv[1], static_cast<DWORD>(current_path.size()),
                         current_path.data(), nullptr);
    Check(length && length < current_path.size(), "exact current head path");
    current_path.resize(length);
    std::replace(current_path.begin(), current_path.end(), L'/', L'\\');
    current_path = L"\\\\?\\" + current_path + L"\\current";
    HANDLE reader = CreateFileW(current_path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(reader != INVALID_HANDLE_VALUE,
          "actual reader pins current head against replacement");
    Check(journal.Publish(old) == 0,
          "same-owner update is idempotent with pinned head");
    CloseHandle(reader);
    auto other_signer = old;
    std::fill_n(other_signer.signer, 64, L'b');
    Check(journal.Publish(other_signer) == ERROR_ACCESS_DENIED,
          "same owner cannot authorize an unrelated selected signer");
    Check(journal.Current(&value, &exists) == 0 && exists &&
              std::wstring_view(value.signer) == old.signer,
          "signer mismatch preserves prior exact journal record");
    Check(journal.Retire(old) == 0,
          "retirement persists before subsequent admission");
    bool retired = false;
    Check(journal.Retired(old.installation, &retired) == 0 && retired,
          "retired owner fenced");
    Check(journal.Publish(old) == ERROR_ACCESS_DENIED,
          "retired owner cannot reenter after revocation");
    Check(journal.Publish(next) == 0, "fresh owner can publish");
    std::vector<ProtectedOwnerRecord> profiles;
    Check(journal.Profiles(old.installation, &profiles) == 0 &&
              profiles.size() == 1,
          "old exact profile identity retained for cleanup");
    auto invalid = old;
    invalid.installation[0] = L'X';
    Check(journal.Publish(invalid) == ERROR_INVALID_PARAMETER,
          "noncanonical owner fails before publication");
  }
  {
    ProtectedOwnerJournal restarted;
    Check(restarted.Open(argv[1]) == 0,
          "simulated authority restart opens same state");
    ProtectedOwnerRecord value{};
    bool exists = false;
    Check(restarted.Current(&value, &exists) == 0 && exists &&
              std::wstring_view(value.installation) == next.installation,
          "new exact owner survives restart");
    Check(restarted.Publish(old) == ERROR_ACCESS_DENIED,
          "restart cannot resurrect old installation");
    std::vector<ProtectedOwnerRecord> history;
    Check(restarted.Profiles(old.installation, &history) == 0 &&
              history.size() == 1,
          "old record remains after restart");
  }
  std::puts(
      "PASS actual Windows owner journal: concurrent exclusion, same-owner "
      "update, retire-before-reentry, fresh owner, durable restart and "
      "retained old records");
  return 0;
}
