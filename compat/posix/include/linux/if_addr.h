// gesher-base: <linux/if_addr.h>.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrmsg {
  uint8_t ifa_family;
  uint8_t ifa_prefixlen;
  uint8_t ifa_flags;
  uint8_t ifa_scope;
  uint32_t ifa_index;
};

enum {
  IFA_UNSPEC,
  IFA_ADDRESS,
  IFA_LOCAL,
  IFA_LABEL,
  IFA_BROADCAST,
  IFA_ANYCAST,
  IFA_CACHEINFO,
  IFA_MULTICAST,
  IFA_FLAGS,
  __IFA_MAX,
};
#define IFA_MAX (__IFA_MAX - 1)

#define IFA_F_SECONDARY   0x01
#define IFA_F_TEMPORARY   IFA_F_SECONDARY
#define IFA_F_NODAD       0x02
#define IFA_F_OPTIMISTIC  0x04
#define IFA_F_DADFAILED   0x08
#define IFA_F_HOMEADDRESS 0x10
#define IFA_F_DEPRECATED  0x20
#define IFA_F_TENTATIVE   0x40
#define IFA_F_PERMANENT   0x80

#ifndef IFA_RTA
#define IFA_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ifaddrmsg))))
#endif
#ifndef IFA_PAYLOAD
#define IFA_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct ifaddrmsg))
#endif

#ifdef __cplusplus
}
#endif
