// gesher-base: descriptors and names of Gesher's /proc (procfs.cc), for the
// bionic shim's own path and statfs handling.
#pragma once

#include <string>

namespace gesher {

// Whether fd is open on a /proc (or /sys) file or directory: its Linux path
// ("/proc/1234/maps") and whether it is a directory.
bool ProcfsDescriptor(int fd, std::string* linux_path, bool* directory);

// Whether path names an existing /proc (or /sys) file, directory or link: its
// Linux path, with /proc/self resolved.
bool ProcfsName(const char* path, std::string* linux_path);

}  // namespace gesher
