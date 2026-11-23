// gesher-base: <sys/ioctl.h>. Device ioctls do not exist on Windows; ioctl
// fails with ENOTTY.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD 0x541B
#define FIONBIO  0x5421
#define SIOCINQ  FIONREAD

// Interface ioctls on a socket (<net/if.h>). SIOCGIFCONF takes an ifconf;
// the other requests take an ifreq naming the interface.
#define SIOCGIFCONF  0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCGIFADDR  0x8915
#define SIOCGIFMTU   0x8921
#define SIOCGIFINDEX 0x8933

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
