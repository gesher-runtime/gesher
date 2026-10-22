// gesher-base: <netinet/tcp.h>, Linux's option numbers. TCP_NODELAY and the
// keepalive options map to Winsock's; the others fail with ENOPROTOOPT.
#pragma once

#include <netinet/in.h>

#define TCP_NODELAY      1
#define TCP_MAXSEG       2
#define TCP_CORK         3
#define TCP_KEEPIDLE     4
#define TCP_KEEPINTVL    5
#define TCP_KEEPCNT      6
#define TCP_SYNCNT       7
#define TCP_LINGER2      8
#define TCP_DEFER_ACCEPT 9
#define TCP_WINDOW_CLAMP 10
#define TCP_INFO         11
#define TCP_QUICKACK     12
#define TCP_CONGESTION   13
#define TCP_USER_TIMEOUT 18
#define TCP_FASTOPEN     23
#define TCP_NOTSENT_LOWAT 25
