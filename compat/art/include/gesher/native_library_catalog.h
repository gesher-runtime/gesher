/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <gesher/elf_loader.h>

#include <string>
#include <vector>

namespace gesher::nativeloader {

// A declaration backed by an accessible, ABI-identified ELF file. These are
// files, not virtual providers: the ordinary ELF loader opens their dependency
// graphs when a classloader actually requests them.
struct PublicLibraryFile {
  std::string soname;
  std::string path;
  std::string directory;
  elf::ProviderVisibility visibility;
  elf::Architecture architecture;
  bool vendor;
};

// Reads the same installed public.libraries configuration for the package
// manager and for classloader namespace links. Missing files are unavailable;
// unreadable or invalid configuration is an error, never an empty capability.
// system_root is ANDROID_ROOT, or /system when it is not configured.
bool ReadPublicLibraryFiles(const std::string& system_root,
                            std::vector<PublicLibraryFile>* files, std::string* error);
std::string SystemRoot();

}  // namespace gesher::nativeloader
