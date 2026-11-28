// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

// Test-only API failure/identity boundaries. Successful observations and both
// compared file identities come from real, separately opened Windows files.
namespace {
bool deny_reopen = false;
bool deny_identity = false;
HANDLE substitute = INVALID_HANDLE_VALUE;
HANDLE OpenCandidate(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                     DWORD disposition, DWORD flags, HANDLE model) {
  if (deny_reopen) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
  if (substitute != INVALID_HANDLE_VALUE) {
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), substitute, GetCurrentProcess(), &duplicate,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) return INVALID_HANDLE_VALUE;
    return duplicate;
  }
  return CreateFileW(name, access, share, security, disposition, flags, model);
}
BOOL QueryInformation(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, LPVOID result, DWORD size) {
  if (deny_identity && type == FileIdInfo) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
  return GetFileInformationByHandleEx(file, type, result, size);
}
}  // namespace

#define CreateFileW OpenCandidate
#define GetFileInformationByHandleEx QueryInformation
#include <gesher/canonical_file_name.h>
#undef GetFileInformationByHandleEx
#undef CreateFileW

int wmain(int argc, wchar_t** argv) {
  if (argc != 2 && argc != 3) return 2;
  const std::wstring parent = std::wstring(argv[1]) + L"\\CanonicalParent";
  const std::wstring leaf = parent + L"\\CanonicalLeaf.txt";
  const std::wstring other = parent + L"\\Other.txt";
  if (!CreateDirectoryW(parent.c_str(), nullptr)) return 3;
  HANDLE first = CreateFileW(leaf.c_str(), GENERIC_READ | GENERIC_WRITE, 7, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE second = CreateFileW(other.c_str(), GENERIC_READ | GENERIC_WRITE, 7, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (first == INVALID_HANDLE_VALUE || second == INVALID_HANDLE_VALUE) return 4;
  unsigned checks = 0, failures = 0;
  auto check = [&](bool ok, const char* text) {
    ++checks; if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
  };
  std::wstring name;
  std::wstring alias = std::wstring(argv[1]) + L"\\canonicalparent\\canonicalleaf.TXT";
  check(gesher::canonical_file_name::RevalidatedDosName(first, alias, &name) && name == leaf,
        "real parent and leaf case restored with full identity");
  check(gesher::canonical_file_name::RevalidatedDosName(first, L"\\\\?\\" + alias, &name) && name == leaf,
        "production MapPath extended DOS hint restores real casing");
  check(gesher::canonical_file_name::RevalidatedDosName(first, alias.substr(2), &name) && name == leaf,
        "production MapPath current-drive rooted hint restores real casing");
  const std::wstring long_parent = parent + L"\\" + std::wstring(120, L'a');
  const std::wstring long_child = long_parent + L"\\" + std::wstring(120, L'b');
  const std::wstring long_leaf = long_child + L"\\LongLeaf.txt";
  const std::wstring extended_parent = L"\\\\?\\" + long_parent;
  const std::wstring extended_child = L"\\\\?\\" + long_child;
  const std::wstring extended_leaf = L"\\\\?\\" + long_leaf;
  const bool parents_created = CreateDirectoryW(extended_parent.c_str(), nullptr) &&
                               CreateDirectoryW(extended_child.c_str(), nullptr);
  HANDLE long_file = parents_created ? CreateFileW(extended_leaf.c_str(), GENERIC_READ, 7, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
  check(long_file != INVALID_HANDLE_VALUE && long_leaf.size() > MAX_PATH &&
        gesher::canonical_file_name::RevalidatedDosName(long_file, extended_leaf, &name) && name == long_leaf,
        "actual path longer than MAX_PATH reopens with extended DOS syntax");
  if (long_file != INVALID_HANDLE_VALUE) CloseHandle(long_file);
  check(DeleteFileW(extended_leaf.c_str()) && RemoveDirectoryW(extended_child.c_str()) &&
        RemoveDirectoryW(extended_parent.c_str()), "owned long-path file and parents removed");
  if (argc == 3) {
    const std::wstring substituted = std::wstring(argv[2]) + L"\\CanonicalParent\\CanonicalLeaf.txt";
    HANDLE via_subst = CreateFileW(substituted.c_str(), 0, 7, nullptr, OPEN_EXISTING, 0, nullptr);
    name = L"unchanged";
    check(via_subst != INVALID_HANDLE_VALUE &&
          !gesher::canonical_file_name::RevalidatedDosName(via_subst, substituted, &name) &&
          name == L"unchanged", "SUBST drive cannot publish an unverified volume projection");
    if (via_subst != INVALID_HANDLE_VALUE) CloseHandle(via_subst);
  }
  wchar_t short_name[32768];
  const DWORD short_size = GetShortPathNameW(leaf.c_str(), short_name, 32768);
  if (short_size && short_size < 32768 && leaf != short_name) {
    check(gesher::canonical_file_name::RevalidatedDosName(first, short_name, &name) && name == leaf,
          "real existing 8.3 alias expands to canonical name");
  } else std::puts("UNAVAILABLE 8.3 aliases are not enabled for this owned file");
  for (const auto* invalid : {L"relative", L"C:relative", L"\\\\server\\share\\file",
                            L"\\\\?\\UNC\\server\\share\\file", L"\\\\.\\C:\\file"}) {
    name = L"unchanged";
    check(!gesher::canonical_file_name::RevalidatedDosName(first, invalid, &name) && name == L"unchanged",
          "unsupported original hint fails without publishing a name");
  }
  deny_reopen = true;
  check(!gesher::canonical_file_name::RevalidatedDosName(first, leaf, &name),
        "current-token metadata reopen denial fails closed");
  deny_reopen = false;
  deny_identity = true;
  check(!gesher::canonical_file_name::RevalidatedDosName(first, leaf, &name),
        "unsupported full identity fails closed");
  deny_identity = false;
  substitute = second;
  check(!gesher::canonical_file_name::RevalidatedDosName(first, leaf, &name),
        "different real file identity is rejected");
  substitute = INVALID_HANDLE_VALUE;
  const std::wstring link = parent + L"\\Link.txt";
  if (CreateSymbolicLinkW(link.c_str(), leaf.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    HANDLE reparse = CreateFileW(link.c_str(), 0, 7, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    substitute = reparse;
    check(reparse != INVALID_HANDLE_VALUE &&
          !gesher::canonical_file_name::RevalidatedDosName(first, leaf, &name),
          "a real final reparse object cannot substitute for the held file");
    substitute = INVALID_HANDLE_VALUE;
    if (reparse != INVALID_HANDLE_VALUE) CloseHandle(reparse);
    check(DeleteFileW(link.c_str()) != FALSE, "owned symbolic link removed");
  } else std::puts("UNAVAILABLE unprivileged symbolic-link creation is not enabled");
  DWORD before = 0, after = 0;
  const bool measured = GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE;
  for (int index = 0; index != 256; ++index) {
    if (!gesher::canonical_file_name::RevalidatedDosName(first, leaf, &name)) ++failures;
  }
  check(measured && GetProcessHandleCount(GetCurrentProcess(), &after) && before == after,
        "256 successful metadata reopen cycles leak no kernel handles");
  CloseHandle(first);
  CloseHandle(second);
  check(DeleteFileW(leaf.c_str()) && DeleteFileW(other.c_str()) && RemoveDirectoryW(parent.c_str()),
        "all owned files and directory closed and removed");
  std::printf("canonical file name: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
