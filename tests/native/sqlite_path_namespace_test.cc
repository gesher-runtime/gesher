// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Portable fixture boundary only. This does not attach a Windows drop channel
// or grant access to a brokered file; the mapper below is extracted unchanged.
#include "sqlite_path_namespace.h"

#include <cstdio>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
}

int main() {
  using gesher::PathKind;
  Check(std::string_view(gesher::drop_mount::kAndroidPrefix) == "/mnt/gesher/drop/",
        "drop prefix comes from the actual declared namespace");
  Check(!gesher::drop_mount::Enabled(), "portable fixture has no attached drop channel");
  std::wstring mapped;
  Check(gesher::MapPath("/mnt/gesher/drop/7/item.sqlite", &mapped) == PathKind::kFile &&
        mapped == L"/mnt/gesher/drop/7/item.sqlite",
        "unattached drop spelling retains ordinary lexical mapping");
  Check(gesher::MapPath("/mnt/gesher/drop/7/a:b?.sqlite", &mapped) == PathKind::kFile &&
        mapped == L"/mnt/gesher/drop/7/a\uf03ab\uf03f.sqlite",
        "unattached drop spelling still encodes reserved characters");
  Check(gesher::MapPath("/mnt/gesher/dropper/7/a:b?.sqlite", &mapped) == PathKind::kFile &&
        mapped == L"/mnt/gesher/dropper/7/a\uf03ab\uf03f.sqlite",
        "prefix lookalike retains ordinary lexical mapping");
  const std::string long_drop = "/mnt/gesher/drop/7/" + std::string(300, 'x');
  Check(gesher::MapPath(long_drop.c_str(), &mapped) == PathKind::kFile &&
        mapped == L"\\\\?\\D:\\mnt\\gesher\\drop\\7\\" + std::wstring(300, L'x'),
        "unattached drop spelling retains existing long-path handling");
  Check(gesher::MapPath("/mnt/c/ordinary.sqlite", &mapped) == PathKind::kFile &&
        mapped == L"C:\\ordinary.sqlite", "ordinary drive mapping is unchanged");
  Check(gesher::MapPath("/dev/null", &mapped) == PathKind::kNull,
        "virtual device remains outside the disk namespace");
  Check(gesher::MapPath("/mnt/unc/server", &mapped) == PathKind::kMissing,
        "malformed UNC remains rejected");
  errno = 0;
  Check(!gesher::DiskPath("/dev/null", &mapped, EINVAL, true) && errno == EINVAL,
        "allow-broker filename mapping does not admit virtual devices");
  errno = 0;
  Check(!gesher::DiskPath(nullptr, &mapped, EINVAL, true) && errno == EFAULT,
        "null filename remains rejected");
  errno = 0;
  Check(!gesher::DiskPath("", &mapped, EINVAL, true) && errno == ENOENT,
        "empty filename remains rejected");
  Check(!gesher::storage_root::Lookup(L"\\mnt\\gesher\\drop\\7\\item.sqlite"),
        "portable fixture supplies no rooted-storage admission");
  std::printf("sqlite-namespace-boundary: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
