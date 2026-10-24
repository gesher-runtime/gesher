// gesher-base: <sys/select.h>, Linux's fd_set (FD_SETSIZE descriptors in a
// bit array) over gesher-base's descriptors: select and pselect wait through
// poll (src/select.cc), so they take files, pipes, sockets, eventfds and
// epoll descriptors alike.
//
// Do not include <winsock2.h> in the same file: it defines its own fd_set
// (an array of SOCKET handles) and select.
#pragma once

#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef unsigned long long __fd_mask;
#define __NFDBITS (8 * (int) sizeof(__fd_mask))

typedef struct {
  __fd_mask fds_bits[FD_SETSIZE / __NFDBITS];
} fd_set;

#define __FD_ELT(fd) ((fd) / __NFDBITS)
#define __FD_MASK(fd) ((__fd_mask) 1 << ((fd) % __NFDBITS))

#define FD_ZERO(set) __builtin_memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) ((void) ((set)->fds_bits[__FD_ELT(fd)] |= __FD_MASK(fd)))
#define FD_CLR(fd, set) ((void) ((set)->fds_bits[__FD_ELT(fd)] &= ~__FD_MASK(fd)))
#define FD_ISSET(fd, set) (((set)->fds_bits[__FD_ELT(fd)] & __FD_MASK(fd)) != 0)

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
           struct timeval* timeout);
int pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
            const struct timespec* timeout, const sigset_t* sigmask);

#ifdef __cplusplus
}
#endif
