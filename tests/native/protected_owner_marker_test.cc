// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Actual Windows marker pin, replacement exclusion and stale-owner controls.
#include "../../compat/usb_storage/protected_owner_marker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
void Check(bool value, const char* what) {
  if (!value) {
    std::fprintf(stderr, "FAIL %s Win32=%lu\n", what, GetLastError());
    std::exit(1);
  }
}
void Directory(const std::wstring& p) {
  Check(CreateDirectoryW(p.c_str(), nullptr) ||
            GetLastError() == ERROR_ALREADY_EXISTS,
        "own fixture directory");
}
void Write(const std::wstring& p, const char* text) {
  HANDLE f =
      CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  Check(f != INVALID_HANDLE_VALUE, "own complete marker");
  DWORD n = 0;
  Check(WriteFile(f, text, (DWORD)strlen(text), &n, nullptr) &&
            n == strlen(text) && FlushFileBuffers(f),
        "durable marker contents");
  CloseHandle(f);
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  using gesher::usb_storage::PinProtectedOwnerMarker;
  Check(argc == 2, "explicit disposable state root");
  std::wstring root(32768, L'\0');
  DWORD length =
      GetFullPathNameW(argv[1], (DWORD)root.size(), root.data(), nullptr);
  Check(length > 0 && length < root.size(), "absolute fixture root");
  root.resize(length);
  Directory(root);
  Directory(root + L"\\system");
  Directory(root + L"\\system\\package-enabled-owners");
  const std::wstring path =
      root + L"\\system\\package-enabled-owners\\fixture.owner";
  const wchar_t* old = L"11111111-2222-3333-4444-555555555555";
  const wchar_t* next = L"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  Write(path, "11111111-2222-3333-4444-555555555555");
  HANDLE pin = nullptr;
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", {}, &pin) ==
                ERROR_INVALID_PARAMETER &&
            !pin,
        "missing owner fails before admission");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner",
                                L"00000000-0000-0000-0000-000000000000",
                                &pin) == ERROR_INVALID_PARAMETER &&
            !pin,
        "zero owner rejected");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", next, &pin) ==
                ERROR_ACCESS_DENIED &&
            !pin,
        "wire owner cannot substitute metadata owner");
  Check(PinProtectedOwnerMarker(root, L"../fixture.owner", old, &pin) ==
                ERROR_INVALID_PARAMETER &&
            !pin,
        "caller cannot escape package marker namespace");
  Check(PinProtectedOwnerMarker(root, L"FIXTURE.owner", old, &pin) ==
                ERROR_ACCESS_DENIED &&
            !pin,
        "different-case package cannot alias the actual marker leaf");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", old, &pin) == 0 && pin,
        "actual matching marker pinned");
  Check(!MoveFileExW(path.c_str(), (path + L".retired").c_str(), 0) &&
            GetLastError() == ERROR_SHARING_VIOLATION,
        "publication/removal cannot replace marker during admission");
  HANDLE writing = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
  Check(writing == INVALID_HANDLE_VALUE &&
            GetLastError() == ERROR_SHARING_VIOLATION,
        "owner bytes cannot mutate during admission");
  CloseHandle(pin);
  pin = nullptr;
  Check(MoveFileExW(path.c_str(), (path + L".retired").c_str(), 0),
        "retire actual marker after pin release");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", old, &pin) ==
                ERROR_FILE_NOT_FOUND &&
            !pin,
        "retired installation cannot be admitted without metadata");
  Write(path, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", old, &pin) ==
                ERROR_ACCESS_DENIED &&
            !pin,
        "new install metadata cannot revive old owner");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", next, &pin) == 0 && pin,
        "fresh owner admitted only after real publication");
  CloseHandle(pin);
  pin = nullptr;
  Check(CreateHardLinkW((path + L".alias").c_str(), path.c_str(), nullptr),
        "own hardlink negative control");
  Check(PinProtectedOwnerMarker(root, L"fixture.owner", next, &pin) ==
                ERROR_ACCESS_DENIED &&
            !pin,
        "multiply linked marker rejected");
  std::puts(
      "PASS actual Windows owner marker: missing/bad/stale owner denial, exact "
      "metadata match, pinned write/rename exclusion, retirement, fresh "
      "publication and hardlink denial");
  return 0;
}
