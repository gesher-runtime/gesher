// gesher-base: <linux/rtnetlink.h> (see <linux/netlink.h>).
#pragma once

#include <linux/netlink.h>
#include <linux/if_addr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTM_NEWLINK  16
#define RTM_DELLINK  17
#define RTM_GETLINK  18
#define RTM_NEWADDR  20
#define RTM_DELADDR  21
#define RTM_GETADDR  22
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26

#define RTMGRP_LINK        1
#define RTMGRP_IPV4_IFADDR 0x10
#define RTMGRP_IPV6_IFADDR 0x100
#define RTMGRP_IPV4_ROUTE  0x40
#define RTMGRP_IPV6_ROUTE  0x400
#define RTMGRP_NEIGH       4

enum rt_scope_t {
  RT_SCOPE_UNIVERSE = 0,
  RT_SCOPE_SITE = 200,
  RT_SCOPE_LINK = 253,
  RT_SCOPE_HOST = 254,
  RT_SCOPE_NOWHERE = 255,
};

struct rtattr {
  unsigned short rta_len;
  unsigned short rta_type;
};

#define RTA_ALIGNTO 4U
#define RTA_ALIGN(len) (((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_OK(rta, len) \
  ((len) >= (int)sizeof(struct rtattr) && \
   (rta)->rta_len >= sizeof(struct rtattr) && \
   (unsigned int)(rta)->rta_len <= (unsigned int)(len))
#define RTA_NEXT(rta, attrlen) \
  ((attrlen) -= RTA_ALIGN((rta)->rta_len), \
   (struct rtattr*)(((char*)(rta)) + RTA_ALIGN((rta)->rta_len)))
#define RTA_LENGTH(len) (RTA_ALIGN(sizeof(struct rtattr)) + (len))
#define RTA_SPACE(len) RTA_ALIGN(RTA_LENGTH(len))
#define RTA_DATA(rta) ((void*)(((char*)(rta)) + RTA_LENGTH(0)))
#define RTA_PAYLOAD(rta) ((int)((rta)->rta_len) - RTA_LENGTH(0))

struct rtgenmsg {
  unsigned char rtgen_family;
};

struct ifinfomsg {
  unsigned char ifi_family;
  unsigned char __ifi_pad;
  unsigned short ifi_type;
  int ifi_index;
  unsigned int ifi_flags;
  unsigned int ifi_change;
};

enum {
  IFLA_UNSPEC,
  IFLA_ADDRESS,
  IFLA_BROADCAST,
  IFLA_IFNAME,
  IFLA_MTU,
  IFLA_LINK,
  IFLA_QDISC,
  IFLA_STATS,
  __IFLA_MAX,
};
#define IFLA_MAX (__IFLA_MAX - 1)

#define IFLA_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ifinfomsg))))
#define IFLA_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct ifinfomsg))

#ifdef __cplusplus
}
#endif
