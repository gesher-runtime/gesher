// gesher-base internals for POSIX FIFOs (fifo.cc).
#pragma once

#include <windows.h>

#include <string>

struct stat;

namespace gesher {

// True when `windows` names an existing FIFO marker file.
bool IsFifoPath(const std::wstring& windows);

// If `existing_handle` (opened for `windows` with POSIX `flags`) is a FIFO
// marker file, closes `existing_handle` and performs FIFO open semantics:
// returns true with `*out_fd >= 0`, or true with `*out_fd == -1` and `errno` set
// (e.g. ENXIO for O_WRONLY | O_NONBLOCK with no reader). Returns false without
// touching `existing_handle` when `existing_handle` is an ordinary file.
bool TryOpenFifo(const std::wstring& windows, HANDLE existing_handle, int flags, int* out_fd);

bool IsFifoFd(int fd);
bool ReadFifo(int fd, void* buf, unsigned int count, int* result);
bool WriteFifo(int fd, const void* buf, unsigned int count, int* result);
int FifoWriteError(int fd);
bool PollFifo(int fd, short events, short* revents);
bool FstatFifo(int fd, struct stat* buf);
bool FifoAvailable(int fd, int* bytes);
bool FcntlFifo(int fd, int cmd, long arg, int* result);
void DupFifo(int fd, int fd2);
bool ForgetFifo(int fd);

}  // namespace gesher
