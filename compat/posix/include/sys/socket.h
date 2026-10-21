// gesher-base: <sys/socket.h>, Linux's socket ABI over Winsock.
//
// Constants and structures are Linux's (AF_INET6 == 10, SOL_SOCKET == 1,
// ...), because Android's Java libraries hard-code them. Sockets are file
// descriptors: read/write/close/poll/fcntl/ioctl accept them. src/socket.cc
// translates to Winsock. See docs/decisions/0009-socket-fds.md.
//
// Do not include <winsock2.h> in the same file: its declarations of the same
// functions take SOCKET handles and Windows' constants.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

#define AF_UNSPEC  0
#define AF_UNIX    1
#define AF_LOCAL   AF_UNIX
#define AF_INET    2
#define AF_INET6   10
#define AF_NETLINK 16
#define AF_PACKET  17
#define AF_VSOCK   40
#define AF_MAX     46
#define PF_UNSPEC  AF_UNSPEC
#define PF_UNIX    AF_UNIX
#define PF_LOCAL   AF_LOCAL
#define PF_INET    AF_INET
#define PF_INET6   AF_INET6
#define PF_NETLINK AF_NETLINK
#define PF_PACKET  AF_PACKET

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5
#define SOCK_PACKET    10
#define SOCK_NONBLOCK  00004000
#define SOCK_CLOEXEC   02000000

#define SOL_SOCKET  1
#define SOL_IP      0
#define SOL_TCP     6
#define SOL_UDP     17
#define SOL_IPV6    41
#define SOL_ICMPV6  58
#define SOL_PACKET  263
#define SOL_NETLINK 270

#define SO_DEBUG       1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_DONTROUTE   5
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_OOBINLINE   10
#define SO_NO_CHECK    11
#define SO_PRIORITY    12
#define SO_LINGER      13
#define SO_BSDCOMPAT   14
#define SO_REUSEPORT   15
#define SO_PASSCRED    16
#define SO_PEERCRED    17
#define SO_RCVLOWAT    18
#define SO_SNDLOWAT    19
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_BINDTODEVICE 25
#define SO_TIMESTAMP   29
#define SO_ACCEPTCONN  30
#define SO_PEERSEC     31
#define SO_SNDBUFFORCE 32
#define SO_RCVBUFFORCE 33
#define SO_PASSSEC     34
#define SO_TIMESTAMPNS 35
#define SO_MARK        36
#define SO_PROTOCOL    38
#define SO_DOMAIN      39
#define SO_BINDTOIFINDEX 62

#define SOMAXCONN 4096

#define MSG_OOB          0x01
#define MSG_PEEK         0x02
#define MSG_DONTROUTE    0x04
#define MSG_CTRUNC       0x08
#define MSG_TRUNC        0x20
#define MSG_DONTWAIT     0x40
#define MSG_EOR          0x80
#define MSG_WAITALL      0x100
#define MSG_CONFIRM      0x800
#define MSG_ERRQUEUE     0x2000
#define MSG_NOSIGNAL     0x4000
#define MSG_MORE         0x8000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SCM_RIGHTS      0x01
#define SCM_CREDENTIALS 0x02

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_storage {
  sa_family_t ss_family;
  char __ss_padding[128 - sizeof(sa_family_t) - sizeof(unsigned long long)];
  unsigned long long __ss_align;
};

struct linger {
  int l_onoff;
  int l_linger;
};

struct ucred {
  pid_t pid;
  uid_t uid;
  gid_t gid;
};

struct msghdr {
  void* msg_name;
  socklen_t msg_namelen;
  struct iovec* msg_iov;
  size_t msg_iovlen;
  void* msg_control;
  size_t msg_controllen;
  int msg_flags;
};

struct mmsghdr {
  struct msghdr msg_hdr;
  unsigned int msg_len;
};

// Ancillary data is not supported: recvmsg returns msg_controllen == 0 and
// sendmsg ignores msg_control.
struct cmsghdr {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg) ((unsigned char*)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(mhdr) \
  ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr*)(mhdr)->msg_control \
                                                     : (struct cmsghdr*)0)
#define CMSG_NXTHDR(mhdr, cmsg) __cmsg_nxthdr((mhdr), (cmsg))
struct cmsghdr* __cmsg_nxthdr(struct msghdr* mhdr, struct cmsghdr* cmsg);

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const struct sockaddr* addr, socklen_t len);
int connect(int fd, const struct sockaddr* addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* len);
int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);
int getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int getpeername(int fd, struct sockaddr* addr, socklen_t* len);
int getsockopt(int fd, int level, int name, void* value, socklen_t* len);
int setsockopt(int fd, int level, int name, const void* value, socklen_t len);
ssize_t send(int fd, const void* buf, size_t len, int flags);
ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* addr,
               socklen_t addrlen);
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* addr,
                 socklen_t* addrlen);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);
int shutdown(int fd, int how);

// gesher-base: whether fd is a socket.
int gesher_is_socket(int fd);

#ifdef __cplusplus
}
#endif
