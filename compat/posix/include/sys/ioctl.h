// gesher-base: <sys/ioctl.h>. Device ioctls do not exist on Windows; ioctl
// fails with ENOTTY.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD 0x541B
#define FIONBIO  0x5421
#define SIOCINQ  FIONREAD

// Interface ioctls on a socket, with a struct ifreq (<net/if.h>) naming the
// interface; answered from the interface list (src/netdb.cc).
#define SIOCGIFFLAGS 0x8913
#define SIOCGIFMTU   0x8921

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
