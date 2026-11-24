// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <gesher/rooted_storage_wire.h>

#include <string>
#include <optional>

namespace gesher::storage_root {

struct Selection {
  bool matched = false;
  int error = 0;
  uint32_t area = rooted_storage_wire::kCredential;
  std::wstring relative;
  uint64_t drop_alias = 0;

  // Failure to classify a path in a contained process also suppresses fallback.
  explicit operator bool() const { return matched || error != 0; }
};

// Accepts a Windows path from MapPath, including its encoded on-disk names.
Selection Lookup(const std::wstring& mapped);

// Resolves a real handle's kernel name to the exact rooted DOS name. A lowbox
// can query the NT object name even when Win32 drive translation is denied.
std::wstring DescriptorPath(HANDLE file);

// Match only the caller-selected original bootstrap file.
std::optional<rooted_storage_wire::Op> LookupBootstrap(const std::wstring& mapped);
int OpenBootstrap(rooted_storage_wire::Op operation, HANDLE* file);

// Installed guests query the device broker. Standalone guests use their
// rooted-storage host when one is present.
bool HasBroker();
int StatAndroidData(rooted_storage_wire::Filesystem* filesystem);

// Return 0 or positive Linux errno. A successful Open returns a real, owned,
// non-inheritable kernel handle, never a reusable numeric protocol identifier.
int Open(const Selection& path, uint32_t flags, HANDLE* file, bool* created = nullptr);
int MakeDirectory(const Selection& path);
int Remove(const Selection& path, bool directory);
int Rename(const Selection& source, const Selection& target, bool replace);

}  // namespace gesher::storage_root
