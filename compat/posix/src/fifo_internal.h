// gesher-base internals for POSIX FIFOs (fifo.cc, ADR 0097).
//
// A FIFO is a file with the IO_REPARSE_TAG_LX_FIFO reparse tag, as WSL
// creates them. Opening one yields a descriptor of kind kFifo: a pipe-type
// placeholder handle that carries no data, and an open file description whose
// shared section has a name, so that inherited copies of it in other
// processes keep the endpoint open. The bytes live in one shared ring per
// FIFO (a named section and mutex keyed by volume serial and file ID), so
// independent opens in any process of the same session (or AppContainer)
// rendezvous on the same pipe.
#pragma once

#include <windows.h>
#include <gesher/file_description.h>

#include <cstdint>
#include <string>

namespace gesher {

// WSL's FIFO tag. Opening such a file without FILE_FLAG_OPEN_REPARSE_POINT
// fails with ERROR_CANT_ACCESS_FILE, so no ordinary open can truncate it.
inline constexpr DWORD kReparseTagFifo = 0x80000024;  // IO_REPARSE_TAG_LX_FIFO

// True when `file` (opened with FILE_FLAG_OPEN_REPARSE_POINT and at least
// FILE_READ_ATTRIBUTES) is a FIFO.
bool IsFifoHandle(HANDLE file);

// CreateFileW that also reaches a FIFO when the path is followed: Windows
// refuses to follow its reparse tag, so the FIFO itself is opened instead.
// Other reparse points keep their original failure.
HANDLE CreateFileFollowingFifo(const wchar_t* path, DWORD access, DWORD share,
                               DWORD flags_and_attributes);

// Opens the FIFO at `path`, following symbolic links of its last component,
// with FILE_FLAG_OPEN_REPARSE_POINT; ERROR_CANT_ACCESS_FILE if it is none.
HANDLE OpenFifoFollowing(const std::wstring& path, DWORD access, DWORD share, DWORD flags_and_attributes,
                         SECURITY_ATTRIBUTES* security);

// The FIFO file of a FIFO descriptor, reopened by the path it was opened by
// and checked against its identity; EOPNOTSUPP once renamed or removed.
HANDLE OpenFifoFile(const DescriptorSnapshotPtr& snapshot, DWORD access);

// Opens the FIFO named by `probe` (a FILE_READ_ATTRIBUTES handle to it, which
// this consumes) with POSIX open `flags`, waiting for a peer as Linux does.
// Returns the new descriptor, or -1 with errno.
int OpenFifoHandle(HANDLE probe, int flags);

// Descriptor operations for DescriptorKind::kFifo (pipe_io.cc dispatches).
// `canceled` and `interrupted` are optional events that end a blocking wait
// with EINTR, as for kernel pipes.
int TransferFifo(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count, bool writing,
                 HANDLE canceled, HANDLE interrupted);
int PollFifo(const DescriptorSnapshotPtr& snapshot, short events, short* ready);
int FifoCapacity(const DescriptorSnapshotPtr& snapshot);
int FifoBytesAvailable(const DescriptorSnapshotPtr& snapshot, int* bytes);

// What fstat reports for a FIFO descriptor: the FIFO file's identity and
// permission bits as seen when it was opened.
struct FifoFileStatus {
  uint64_t device = 0, inode = 0;
  uint32_t mode = 0;  // permission bits
  int64_t access_time = 0, write_time = 0, change_time = 0;  // FILETIME units
};
int FifoStatus(const DescriptorSnapshotPtr& snapshot, FifoFileStatus* status);

}  // namespace gesher
