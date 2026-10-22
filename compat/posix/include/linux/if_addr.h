// gesher-base: <linux/if_addr.h>.
#pragma once

#include <stdint.h>

struct ifaddrmsg {
  uint8_t ifa_family;
  uint8_t ifa_prefixlen;
  uint8_t ifa_flags;
  uint8_t ifa_scope;
  uint32_t ifa_index;
};

#define IFA_F_SECONDARY   0x01
#define IFA_F_TEMPORARY   IFA_F_SECONDARY
#define IFA_F_NODAD       0x02
#define IFA_F_OPTIMISTIC  0x04
#define IFA_F_DADFAILED   0x08
#define IFA_F_HOMEADDRESS 0x10
#define IFA_F_DEPRECATED  0x20
#define IFA_F_TENTATIVE   0x40
#define IFA_F_PERMANENT   0x80
