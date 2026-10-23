// gesher-base: /dev/binder (binder.cc), as the fd layer sees it.
#pragma once

#include <stddef.h>

namespace gesher {

// Whether /dev/binder exists: $GESHER_BINDER names a device a binderd serves.
bool BinderDeviceExists();

// open("/dev/binder"): a new fd connected to the broker, or -1 with errno
// (ENOENT when there is no device).
int OpenBinder(int flags);

bool IsBinderFd(int fd);

// mmap of a binder fd: the anonymous mapping (mman.cc) where binder.cc puts
// the transactions the process receives.
void SetBinderMapping(int fd, void* addr, size_t size);

// ioctl on a binder fd; false, doing nothing, if fd is not one.
bool BinderIoctl(int fd, unsigned long request, void* arg, int* result);

// close: drops fd's reference to its device; false if fd is not a binder fd.
bool ForgetBinder(int fd);

// After `fd2` was made a CRT duplicate of `fd`: fd2 shares fd's device.
void DupBinder(int fd, int fd2);

}  // namespace gesher
