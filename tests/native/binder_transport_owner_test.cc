// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/posix/src/binder_transport.h"
#include "../../compat/binder/protected_data_pipe.h"

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  const std::wstring mode(argv[2]);
  const bool ordinary = mode == L"ordinary";
  const bool invalid = mode == L"invalid";
  if (!ordinary && !invalid && mode != L"protected") return 2;
  const std::wstring locator = std::wstring(gesher::binder::data_pipe::kLocatorPrefix) +
      std::wstring(gesher::binder::data_pipe::kLocatorHexDigits, L'a');
  if (!ordinary) {
    const DWORD status = gesher::ConfigureProtectedBinder(invalid
        ? gesher::binder::ProtectedDataEndpoint{}
        : gesher::binder::ProtectedDataEndpoint{locator, 12345, 0, 2});
    if (status != (invalid ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS)) return 3;
  }
  const auto* original = gesher::ProtectedBinderEndpoint();
  HMODULE module = LoadLibraryW(argv[1]);
  if (!module) return 4;
  using Read = const gesher::binder::ProtectedDataEndpoint* (*)();
  using Replace = DWORD (*)();
  const auto read = reinterpret_cast<Read>(reinterpret_cast<void*>(
      GetProcAddress(module, "ReadProtectedBinderRoute")));
  const auto replace = reinterpret_cast<Replace>(reinterpret_cast<void*>(
      GetProcAddress(module, "ReplaceProtectedBinderRoute")));
  bool ok = read && replace && read() == original &&
      replace() == ERROR_ALREADY_INITIALIZED &&
      ((original == nullptr) == ordinary);
  if (ok && !ordinary) {
    ok = invalid ? original->locator.empty() && original->authority_pid == 0 :
        original->locator == locator && original->authority_pid == 12345 &&
        original->authority_session == 0 && original->desktop_session == 2;
  }
  FreeLibrary(module);
  std::printf("Binder executable/DLL routing (%ls): %s\n", mode.c_str(), ok ? "PASS" : "FAIL");
  return ok ? 0 : 5;
}
