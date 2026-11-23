// gesher-base: AF_NETLINK / NETLINK_ROUTE internal interface (netlink.cc).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <memory>

#include "socket_internal.h"

namespace gesher {

struct NetlinkState;

std::shared_ptr<NetlinkState> CreateNetlinkState(int sock_type);
void NetlinkClose(NetlinkState& state);
int NetlinkPoll(NetlinkState& state, int events);

int NetlinkConnect(NetlinkState& state, const void* addr, uint32_t len);
int NetlinkBind(NetlinkState& state, const void* addr, uint32_t len);
int NetlinkGetSockName(NetlinkState& state, void* addr, uint32_t* len, bool peer);

ssize_t NetlinkSend(NetlinkState& state, const ws::Buffer* buffers, size_t count, int flags,
                    const void* addr, uint32_t addrlen);
ssize_t NetlinkRecv(NetlinkState& state, ws::Buffer* buffers, size_t count, int flags, void* addr,
                    uint32_t* addrlen, bool nonblocking, int* result_flags = nullptr);

int NetlinkSetSockOpt(NetlinkState& state, int level, int name, const void* value, uint32_t len);
int NetlinkGetSockOpt(NetlinkState& state, int level, int name, void* value, uint32_t* len);

}  // namespace gesher
