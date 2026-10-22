// gesher-base: <linux/netlink.h>. Netlink sockets do not exist on Windows;
// the types are defined for code that formats their addresses.
#pragma once

#include <stdint.h>
#include <sys/socket.h>

#define NETLINK_ROUTE 0

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
