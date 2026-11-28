// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include "dns_peer_ports.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dns_fixture {
namespace peer_port_test {
inline int checks = 0;
inline int callbacks = 0;
inline bool every_attempt = false;
inline std::vector<Socket> colliders;
inline std::vector<Socket> attempts;
inline void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) { std::fprintf(stderr, "FAIL DNS peer ports: %s\n", message); std::exit(1); }
}
inline bool Open(Socket socket) {
  int type = 0;
#ifdef _WIN32
  int size = sizeof(type);
  return ::getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0;
#else
  socklen_t size = sizeof(type);
  return ::getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &size) == 0;
#endif
}
inline void Collide(PeerPorts& pending, const sockaddr_in& address, int attempt) {
  ++callbacks;
  if (!every_attempt && attempt != 1) {
    for (Socket owner : colliders) {
      Check(Open(owner), "failed setup preserves competing owner's socket"); CloseSocket(owner);
    }
    colliders.clear();
    return;
  }
  Socket owner = ::socket(AF_INET, SOCK_STREAM, 0);
  Check(owner != kInvalidSocket, "real collision socket allocated");
  if (::bind(owner, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
    Check(::listen(owner, 1) == 0, "real competing TCP listener established");
    colliders.push_back(owner);
  } else {
    int code = LastSocketError(); CloseSocket(owner);
    // The OS may reuse a UDP port whose TCP address is already held by an
    // earlier collider or another process. That is the same actual collision.
    Check(AddressInUse(code), "controlled TCP reservation or existing native collision");
  }
  attempts.push_back(pending.udp); attempts.push_back(pending.tcp);
}
inline void InvalidTcp(PeerPorts& pending, const sockaddr_in&, int) {
  ++callbacks;
  attempts.push_back(pending.udp);
  CloseSocket(std::exchange(pending.tcp, kInvalidSocket));
}
inline void Clean() {
  for (Socket owner : colliders) {
    Check(Open(owner), "competing owner remains open"); CloseSocket(owner);
  }
  colliders.clear(); attempts.clear(); callbacks = 0; every_attempt = false;
}
inline void AllClosed() {
  for (Socket socket : attempts) {
    // Native handle numbers may be reused for a subsequently opened collider.
    bool reowned = std::find(colliders.begin(), colliders.end(), socket) != colliders.end();
    Check(!Open(socket) || reowned, "failed attempt has no retained unowned socket");
  }
}
inline void Run() {
  PeerPorts result; PeerPortError error;
  Check(ReservePeerPorts(result, error), "ordinary paired reservation succeeds");
  Check(result.port != 0 && Open(result.udp) && Open(result.tcp), "successful reservation owns both sockets");
  Check(error.code == 0 && error.attempts >= 1 && error.attempts <= 16,
        "ordinary attempt count and result");
  result.Reset();
  Check(ReservePeerPorts(result, error, Collide), "real first TCP collision recovers on fresh reservation");
  Check(callbacks >= 2 && callbacks <= 16 && error.attempts == callbacks,
        "real EADDRINUSE forces fresh reservation within the fixed bound");
  Check(result.port != 0 && Open(result.udp) && Open(result.tcp), "recovered pair remains owned");
  result.Reset(); AllClosed(); Clean();
  every_attempt = true;
  Check(!ReservePeerPorts(result, error, Collide), "persistent real collisions fail closed");
  Check(callbacks == 16 && error.attempts == 16,
        "fixed retry ceiling preserves bounded fixture setup");
  Check(AddressInUse(error.code) && std::strcmp(error.operation, "bind TCP") == 0,
        "exhaustion retains native operation and code");
  Check(result.port == 0 && result.udp == kInvalidSocket && result.tcp == kInvalidSocket,
        "failed setup publishes no port or socket");
  AllClosed(); Clean();
  Check(!ReservePeerPorts(result, error, InvalidTcp), "real invalid socket failure propagates");
  Check(callbacks == 1 && error.attempts == 1 && !AddressInUse(error.code),
        "non-collision errors are never retried");
  Check(std::strcmp(error.operation, "bind TCP") == 0 && error.code != 0,
        "non-retryable failure retains operation and native error");
  AllClosed(); Clean();
  Check(ReservePeerPorts(result, error), "normal ownership can be established after failures");
  Socket udp = result.udp, tcp = result.tcp;
  result.Reset(); Check(!Open(udp) && !Open(tcp), "successful sockets also retire explicitly");
  std::printf("DNS peer port reservation controls passed: %d\n", checks);
}
}  // namespace peer_port_test
}  // namespace dns_fixture
