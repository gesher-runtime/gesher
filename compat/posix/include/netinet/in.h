// gesher-base: <netinet/in.h>, Linux's definitions (see <sys/socket.h>).
#pragma once

#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

#define IPPROTO_IP      0
#define IPPROTO_HOPOPTS 0
#define IPPROTO_ICMP    1
#define IPPROTO_IGMP    2
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ESP     50
#define IPPROTO_AH      51
#define IPPROTO_ICMPV6  58
#define IPPROTO_NONE    59
#define IPPROTO_DSTOPTS 60
#define IPPROTO_SCTP    132
#define IPPROTO_UDPLITE 136
#define IPPROTO_RAW     255

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

struct in_addr {
  in_addr_t s_addr;
};

struct in6_addr {
  union {
    uint8_t __u6_addr8[16];
    uint16_t __u6_addr16[8];
    uint32_t __u6_addr32[4];
  } __in6_u;
};
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32

struct sockaddr_in {
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

struct sockaddr_in6 {
  sa_family_t sin6_family;
  in_port_t sin6_port;
  uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  uint32_t sin6_scope_id;
};

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}}
#define IN6ADDR_LOOPBACK_INIT {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}}

struct ip_mreq {
  struct in_addr imr_multiaddr;
  struct in_addr imr_interface;
};

struct ip_mreqn {
  struct in_addr imr_multiaddr;
  struct in_addr imr_address;
  int imr_ifindex;
};

struct ip_mreq_source {
  struct in_addr imr_multiaddr;
  struct in_addr imr_interface;
  struct in_addr imr_sourceaddr;
};

struct ipv6_mreq {
  struct in6_addr ipv6mr_multiaddr;
  unsigned int ipv6mr_interface;
};

struct group_req {
  uint32_t gr_interface;
  struct sockaddr_storage gr_group;
};

struct group_source_req {
  uint32_t gsr_interface;
  struct sockaddr_storage gsr_group;
  struct sockaddr_storage gsr_source;
};

struct in_pktinfo {
  int ipi_ifindex;
  struct in_addr ipi_spec_dst;
  struct in_addr ipi_addr;
};

struct in6_pktinfo {
  struct in6_addr ipi6_addr;
  int ipi6_ifindex;
};

#define IP_TOS             1
#define IP_TTL             2
#define IP_HDRINCL         3
#define IP_OPTIONS         4
#define IP_RECVOPTS        6
#define IP_PKTINFO         8
#define IP_MTU_DISCOVER    10
#define IP_RECVERR         11
#define IP_RECVTTL         12
#define IP_RECVTOS         13
#define IP_MTU             14
#define IP_FREEBIND        15
#define IP_TRANSPARENT     19
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36
#define IP_UNBLOCK_SOURCE  37
#define IP_BLOCK_SOURCE    38
#define IP_ADD_SOURCE_MEMBERSHIP  39
#define IP_DROP_SOURCE_MEMBERSHIP 40
#define IP_MULTICAST_ALL   49
#define IP_PMTUDISC_DONT   0
#define IP_PMTUDISC_WANT   1
#define IP_PMTUDISC_DO     2
#define IP_PMTUDISC_PROBE  3

#define IPV6_ADDRFORM        1
#define IPV6_2292PKTINFO     2
#define IPV6_CHECKSUM        7
#define IPV6_UNICAST_HOPS    16
#define IPV6_MULTICAST_IF    17
#define IPV6_MULTICAST_HOPS  18
#define IPV6_MULTICAST_LOOP  19
#define IPV6_ADD_MEMBERSHIP  20
#define IPV6_JOIN_GROUP      IPV6_ADD_MEMBERSHIP
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_LEAVE_GROUP     IPV6_DROP_MEMBERSHIP
#define IPV6_MTU_DISCOVER    23
#define IPV6_MTU             24
#define IPV6_RECVERR         25
#define IPV6_V6ONLY          26
#define IPV6_RECVPKTINFO     49
#define IPV6_PKTINFO         50
#define IPV6_RECVHOPLIMIT    51
#define IPV6_HOPLIMIT        52
#define IPV6_RECVTCLASS      66
#define IPV6_TCLASS          67

#define MCAST_JOIN_GROUP         42
#define MCAST_BLOCK_SOURCE       43
#define MCAST_UNBLOCK_SOURCE     44
#define MCAST_LEAVE_GROUP        45
#define MCAST_JOIN_SOURCE_GROUP  46
#define MCAST_LEAVE_SOURCE_GROUP 47

#define IN6_IS_ADDR_UNSPECIFIED(a) \
  ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr32[2] == 0 && \
   (a)->s6_addr32[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
  ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr32[2] == 0 && \
   (a)->s6_addr32[3] == __builtin_bswap32(1))
#define IN6_IS_ADDR_V4MAPPED(a) \
  ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && \
   (a)->s6_addr32[2] == __builtin_bswap32(0xffff))
#define IN6_IS_ADDR_V4COMPAT(a) \
  ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 && (a)->s6_addr32[2] == 0 && \
   __builtin_bswap32((a)->s6_addr32[3]) > 1)
#define IN6_IS_ADDR_MULTICAST(a) ((a)->s6_addr[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a) \
  ((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a) \
  ((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0xc0)
#define IN6_IS_ADDR_MC_NODELOCAL(a) (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0xf) == 0x1)
#define IN6_IS_ADDR_MC_LINKLOCAL(a) (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0xf) == 0x2)
#define IN6_IS_ADDR_MC_SITELOCAL(a) (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0xf) == 0x5)
#define IN6_IS_ADDR_MC_ORGLOCAL(a)  (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0xf) == 0x8)
#define IN6_IS_ADDR_MC_GLOBAL(a)    (IN6_IS_ADDR_MULTICAST(a) && ((a)->s6_addr[1] & 0xf) == 0xe)
#define IN6_ARE_ADDR_EQUAL(a, b) (memcmp((a), (b), sizeof(struct in6_addr)) == 0)

#define IN_CLASSA(a) ((((in_addr_t)(a)) & 0x80000000) == 0)
#define IN_MULTICAST(a) ((((in_addr_t)(a)) & 0xf0000000) == 0xe0000000)

// Macros, as in glibc, so they work in constant expressions.
#define htons(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define ntohs(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define htonl(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define ntohl(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))

int bindresvport(int fd, struct sockaddr_in* addr);

#ifdef __cplusplus
}
#endif
