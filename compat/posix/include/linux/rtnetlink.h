// gesher-base: <linux/rtnetlink.h> (see <linux/netlink.h>).
#pragma once

#include <linux/netlink.h>
#include <linux/if_addr.h>

#define RTMGRP_LINK        1
#define RTMGRP_IPV4_IFADDR 0x10
#define RTMGRP_IPV6_IFADDR 0x100
#define RTMGRP_IPV4_ROUTE  0x40
#define RTMGRP_IPV6_ROUTE  0x400
#define RTMGRP_NEIGH       4

struct rtattr {
  unsigned short rta_len;
  unsigned short rta_type;
};
