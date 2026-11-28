// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Exact-object migration over an owned Windows tree, never real app data.
#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../compat/framework/app_container.h"
namespace {
void Check(bool v, const char *what) {
  if (!v) {
    std::fprintf(stderr, "FAIL %s Win32=%lu\n", what, GetLastError());
    std::exit(1);
  }
}
void Directory(const std::wstring &p) {
  Check(CreateDirectoryW(p.c_str(), nullptr) ||
            GetLastError() == ERROR_ALREADY_EXISTS,
        "own fixture directory");
}
void File(const std::wstring &p) {
  HANDLE f =
      CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  Check(f != INVALID_HANDLE_VALUE, "own fixture file");
  DWORD n = 0;
  Check(WriteFile(f, "sentinel", 8, &n, nullptr) && n == 8,
        "actual sentinel bytes");
  CloseHandle(f);
}
std::vector<BYTE> Dacl(const std::wstring &p) {
  DWORD n = 0;
  GetFileSecurityW(p.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &n);
  std::vector<BYTE> bytes(n);
  Check(n && GetFileSecurityW(p.c_str(), DACL_SECURITY_INFORMATION,
                              bytes.data(), n, &n),
        "actual canary DACL snapshot");
  return bytes;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  namespace container = gesher::appcontainer;
  Check(argc == 2, "explicit disposable tree");
  std::wstring root(32768, L'\0');
  DWORD n = GetFullPathNameW(argv[1], (DWORD)root.size(), root.data(), nullptr);
  Check(n && n < root.size(), "actual fixture absolute path");
  root.resize(n);
  Directory(root);
  Directory(root + L"/current");
  Directory(root + L"/current/child");
  File(root + L"/current/child/private");
  File(root + L"/foreign-canary");
  PSID old = nullptr;
  Check(ConvertStringSidToSidW(L"S-1-15-2-111-222-333-444-555-666-777", &old),
        "fixture old SID");
  Check(container::Grant(root + L"/current", old, FILE_GENERIC_READ, true) == 0,
        "actual recursive explicit legacy grants");
  Directory(root + L"/current/inherited");
  File(root + L"/current/inherited/private");
  Check(container::RequireNoGrantTree(root + L"/current", old) ==
            ERROR_ACCESS_DENIED,
        "new install cannot attach to old still-canonical userdata");
  Check(container::RequireAppTrusteesTree(root + L"/current", {old}) == 0,
        "same-signer legacy package trustee is allowed for first migration");
  PSID foreign = nullptr;
  Check(
      ConvertStringSidToSidW(L"S-1-15-2-777-666-555-444-333-222-111", &foreign),
      "fixture unknown package SID");
  Check(container::Grant(root + L"/current/child/private", foreign,
                         FILE_GENERIC_READ, false) == 0,
        "actual unknown package trustee control");
  Check(container::RequireAppTrusteesTree(root + L"/current", {old}) ==
            ERROR_ACCESS_DENIED,
        "different-signer first migration cannot adopt old current data");
  Check(container::RevokeTree(root + L"/current", foreign) == 0,
        "remove fixture unknown trustee");
  LocalFree(foreign);
  Check(container::RevokeTree(root + L"/current", old) == 0,
        "exact-tree legacy grant removal");
  Check(container::RequireNoGrantTree(root + L"/current", old) == 0,
        "all explicit descendant legacy grants actually absent");
  Check(container::Grant(root + L"/current/child/private", old,
                         FILE_GENERIC_READ, false) == 0,
        "actual stale leaf only control");
  Check(container::RequireNoGrantTree(root + L"/current", old) ==
            ERROR_ACCESS_DENIED,
        "old leaf cannot hide beneath a fresh root");
  Check(container::RevokeTree(root + L"/current", old) == 0,
        "clear owned stale-leaf control");
  const auto before = Dacl(root + L"/foreign-canary");
  Check(CreateHardLinkW((root + L"/current/child/alias").c_str(),
                        (root + L"/foreign-canary").c_str(), nullptr),
        "actual foreign-object hardlink canary");
  Check(container::RevokeTree(root + L"/current", old) == ERROR_ACCESS_DENIED,
        "unsupported multiply linked descendant fails migration closed");
  Check(container::RequireNoGrantTree(root + L"/current", old) ==
            ERROR_ACCESS_DENIED,
        "audit cannot claim unvisited linked object safe");
  Check(Dacl(root + L"/foreign-canary") == before,
        "outside canary DACL remains byte-for-byte unchanged");
  LocalFree(old);
  std::puts(
      "PASS actual Windows owner tree guards: stale root/leaf denial, exact "
      "descendant revocation, hardlink failure and unchanged foreign canary");
  return 0;
}
