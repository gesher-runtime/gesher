// gesher-base: <poll.h>. Values match both Linux and winsock2, so a file
// that also includes <winsock2.h> sees consistent constants.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd {
  int fd;
  short events;
  short revents;
};

#ifndef POLLIN
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#endif

typedef unsigned long nfds_t;

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
