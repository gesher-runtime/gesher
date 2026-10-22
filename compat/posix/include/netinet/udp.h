// gesher-base: <netinet/udp.h>.
#pragma once

#include <stdint.h>

struct udphdr {
  uint16_t source;
  uint16_t dest;
  uint16_t len;
  uint16_t check;
};
#define UDP_CORK 1
#define UDP_ENCAP 100
