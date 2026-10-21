// gesher-base: conversions between Linux socket types and the neutral ones
// in socket_internal.h. Linux side only (socket.cc, netdb.cc).
#pragma once

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include "socket_internal.h"

namespace gesher::sock {

namespace ws = gesher::ws;

inline ws::Family ToFamily(int family) {
  switch (family) {
    case AF_UNSPEC: return ws::Family::kUnspec;
    case AF_INET: return ws::Family::kInet;
    case AF_INET6: return ws::Family::kInet6;
    case AF_UNIX: return ws::Family::kUnix;
    default: return ws::Family::kOther;
  }
}

inline int FromFamily(ws::Family family) {
  switch (family) {
    case ws::Family::kUnspec: return AF_UNSPEC;
    case ws::Family::kInet: return AF_INET;
    case ws::Family::kInet6: return AF_INET6;
    case ws::Family::kUnix: return AF_UNIX;
    case ws::Family::kOther: break;
  }
  return AF_UNSPEC;
}

// Linux sockaddr to the neutral form; false (errno set) if it is invalid.
inline bool ToAddress(const sockaddr* addr, socklen_t len, ws::Address* out) {
  if (addr == nullptr || len < sizeof(sa_family_t)) return errno = EINVAL, false;
  out->family = ToFamily(addr->sa_family);
  size_t min_len = 0;
  switch (out->family) {
    case ws::Family::kInet: min_len = sizeof(sockaddr_in); break;
    case ws::Family::kInet6: min_len = sizeof(sockaddr_in6); break;
    case ws::Family::kUnix:
    case ws::Family::kUnspec: min_len = sizeof(sa_family_t); break;
    case ws::Family::kOther: return errno = EAFNOSUPPORT, false;
  }
  if (len < min_len) return errno = EINVAL, false;
  size_t data_len = len - sizeof(sa_family_t);
  if (data_len > sizeof(out->data)) data_len = sizeof(out->data);
  memcpy(out->data, reinterpret_cast<const char*>(addr) + sizeof(sa_family_t), data_len);
  out->data_len = data_len;
  return true;
}

// The neutral form to a caller's buffer, truncating as Linux does: *len is
// set to the full length.
inline void FromAddress(const ws::Address& in, sockaddr* addr, socklen_t* len) {
  if (addr == nullptr || len == nullptr) return;
  sockaddr_storage full = {};
  full.ss_family = static_cast<sa_family_t>(FromFamily(in.family));
  memcpy(reinterpret_cast<char*>(&full) + sizeof(sa_family_t), in.data, in.data_len);
  socklen_t full_len = static_cast<socklen_t>(sizeof(sa_family_t) + in.data_len);
  memcpy(addr, &full, *len < full_len ? *len : full_len);
  *len = full_len;
}

inline ws::Type ToType(int type) {
  switch (type) {
    case SOCK_STREAM: return ws::Type::kStream;
    case SOCK_DGRAM: return ws::Type::kDgram;
    case SOCK_RAW: return ws::Type::kRaw;
    case SOCK_SEQPACKET: return ws::Type::kSeqpacket;
    default: return ws::Type::kOther;
  }
}

inline int FromType(ws::Type type) {
  switch (type) {
    case ws::Type::kStream: return SOCK_STREAM;
    case ws::Type::kDgram: return SOCK_DGRAM;
    case ws::Type::kRaw: return SOCK_RAW;
    case ws::Type::kSeqpacket: return SOCK_SEQPACKET;
    case ws::Type::kOther: break;
  }
  return 0;
}

// The interface ioctls (SIOCGIFFLAGS, SIOCGIFMTU) on a struct ifreq; false
// if `request` is not one (netdb.cc).
bool InterfaceIoctl(unsigned long request, void* arg, int* result);

}  // namespace gesher::sock
