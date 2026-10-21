// gesher-base: <sys/un.h>. Windows 10 supports AF_UNIX stream sockets with
// file system paths; Linux's abstract names (a leading NUL) are not supported.
#pragma once

#include <sys/socket.h>

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[108];
};
