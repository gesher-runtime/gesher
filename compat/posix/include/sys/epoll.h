// gesher-base: <sys/epoll.h>, Linux's values and x86-64 layout. epoll fds
// watch eventfds, sockets, pipes and character devices (src/epoll.cc).
#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPOLL_CLOEXEC 02000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLERR       0x008
#define EPOLLHUP       0x010
#define EPOLLRDNORM    0x040
#define EPOLLRDBAND    0x080
#define EPOLLWRNORM    0x100
#define EPOLLWRBAND    0x200
#define EPOLLMSG       0x400
#define EPOLLRDHUP     0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLET        (1u << 31)

typedef union epoll_data {
  void* ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

// Packed on x86-64, as in Linux's ABI: 12 bytes, data at offset 4.
struct epoll_event {
  uint32_t events;
  epoll_data_t data;
} __attribute__((packed));

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
int epoll_pwait(int epfd, struct epoll_event* events, int maxevents, int timeout,
                const sigset_t* sigmask);

#ifdef __cplusplus
}
#endif
