// gesher-base: <sys/uio.h>.
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
  void* iov_base;
  size_t iov_len;
};

#define IOV_MAX 1024

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);
ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset);
ssize_t process_vm_readv(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt,
                         const struct iovec* remote_iov, unsigned long riovcnt,
                         unsigned long flags);

#ifdef __cplusplus
}
#endif
