// gesher-base: descriptors and names of Gesher's /proc (procfs.cc), for the
// bionic shim's own path and statfs handling.
#pragma once

#include <string>

namespace gesher {

// Whether fd is open on a tagged virtual file or directory: its Linux path
// ("/proc/1234/maps", a /sys name, or the admitted USB /dev/block directory)
// and whether it is a directory. The tag does not select a filesystem type.
bool ProcfsDescriptor(int fd, std::string* linux_path, bool* directory);

// Whether path names an existing /proc (or /sys) file, directory or link,
// as statfs sees it, following a link at its end: 1 with its Linux path
// (/proc/self resolved), and for a link *fd for /proc/self/fd/<n> or
// *target for one to a path (fd -1 and target empty otherwise); -1 with
// errno for a link that cannot be followed (map_files: EPERM); 0 for a name
// that is not one of these.
int ProcfsFollow(const char* path, std::string* linux_path, int* fd, std::string* target);

// Whether fd is open on a FIFO: the Windows path of its file, whose volume
// fstatfs reports (Linux: the file system that holds the FIFO's inode).
// False with errno set when the FIFO's file is gone (renamed or removed).
bool FifoFilePath(int fd, std::wstring* windows_path, bool* is_fifo);

}  // namespace gesher
