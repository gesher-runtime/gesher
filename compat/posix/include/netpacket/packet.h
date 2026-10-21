// gesher-base: <netpacket/packet.h>. AF_PACKET sockets do not exist on
// Windows; the address type is defined for code that formats them.
#pragma once

#include <sys/socket.h>

struct sockaddr_ll {
  unsigned short sll_family;
  unsigned short sll_protocol;
  int sll_ifindex;
  unsigned short sll_hatype;
  unsigned char sll_pkttype;
  unsigned char sll_halen;
  unsigned char sll_addr[8];
};
