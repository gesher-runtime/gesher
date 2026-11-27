// gesher-base: <linux/netlink.h>, Linux netlink constants and headers.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETLINK_ROUTE 0
#define NETLINK_KOBJECT_UEVENT 15

struct sockaddr_nl {
  sa_family_t nl_family;
  unsigned short nl_pad;
  uint32_t nl_pid;
  uint32_t nl_groups;
};

struct nlmsghdr {
  uint32_t nlmsg_len;
  uint16_t nlmsg_type;
  uint16_t nlmsg_flags;
  uint32_t nlmsg_seq;
  uint32_t nlmsg_pid;
};

struct nlmsgerr {
  int error;
  struct nlmsghdr msg;
};

#define NLM_F_REQUEST       0x01
#define NLM_F_MULTI         0x02
#define NLM_F_ACK           0x04
#define NLM_F_ECHO          0x08
#define NLM_F_DUMP_INTR     0x10
#define NLM_F_DUMP_FILTERED 0x20

#define NLM_F_ROOT   0x100
#define NLM_F_MATCH  0x200
#define NLM_F_ATOMIC 0x400
#define NLM_F_DUMP   (NLM_F_ROOT | NLM_F_MATCH)

#define NLMSG_ALIGNTO 4U
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN ((int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh) ((void*)(((char*)(nlh)) + NLMSG_HDRLEN))
#define NLMSG_NEXT(nlh, len) \
  ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), \
   (struct nlmsghdr*)(((char*)(nlh)) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(nlh, len) \
  ((len) >= (int)sizeof(struct nlmsghdr) && \
   (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
   (unsigned int)(nlh)->nlmsg_len <= (unsigned int)(len))
#define NLMSG_PAYLOAD(nlh, len) ((int)((nlh)->nlmsg_len) - NLMSG_SPACE((len)))

#define NLMSG_NOOP    0x1
#define NLMSG_ERROR   0x2
#define NLMSG_DONE    0x3
#define NLMSG_OVERRUN 0x4

#define NETLINK_ADD_MEMBERSHIP  1
#define NETLINK_DROP_MEMBERSHIP 2
#define NETLINK_PKTINFO         3
#define NETLINK_BROADCAST_ERROR 4
#define NETLINK_NO_ENOBUFS      5
#define NETLINK_CAP_ACK         10
#define NETLINK_EXT_ACK         11
#define NETLINK_GET_STRICT_CHK  12

#ifdef __cplusplus
}
#endif
