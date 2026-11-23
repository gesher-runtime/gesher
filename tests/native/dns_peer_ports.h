// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dns_fixture {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
inline int LastSocketError() { return WSAGetLastError(); }
inline bool AddressInUse(int error) { return error == WSAEADDRINUSE; }
inline void CloseSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
inline int LastSocketError() { return errno; }
inline bool AddressInUse(int error) { return error == EADDRINUSE; }
inline void CloseSocket(Socket socket) { close(socket); }
#endif

// A UDP ephemeral allocation reserves no TCP port. Keep both reservations owned
// together, and do not publish a port until the TCP listener is ready.
struct PeerPorts {
  Socket udp = kInvalidSocket, tcp = kInvalidSocket;
  uint16_t port = 0;
  PeerPorts() = default;
  PeerPorts(const PeerPorts&) = delete;
  PeerPorts& operator=(const PeerPorts&) = delete;
  ~PeerPorts() { Reset(); }
  void Reset() {
    if (udp != kInvalidSocket) CloseSocket(std::exchange(udp, kInvalidSocket));
    if (tcp != kInvalidSocket) CloseSocket(std::exchange(tcp, kInvalidSocket));
    port = 0;
  }
};
struct PeerPortError { const char* operation = "none"; int code = 0, attempts = 0; };
using BeforeTcpBind = void (*)(PeerPorts&, const sockaddr_in&, int);
constexpr int kPeerPortAttempts = 16;

// The callback is used only by the fixture's real-socket collision controls.
inline bool ReservePeerPorts(PeerPorts& result, PeerPortError& error,
                             BeforeTcpBind before_tcp_bind = nullptr) {
  result.Reset();
  for (int attempt = 1; attempt <= kPeerPortAttempts; ++attempt) {
    PeerPorts pending;
    error.attempts = attempt;
    auto failed = [&](const char* operation) {
      error.operation = operation;
      error.code = LastSocketError();  // Capture before owned socket cleanup.
      return false;
    };
    pending.udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (pending.udp == kInvalidSocket) return failed("socket UDP");
    pending.tcp = ::socket(AF_INET, SOCK_STREAM, 0);
    if (pending.tcp == kInvalidSocket) return failed("socket TCP");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(pending.udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
      return failed("bind UDP");
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (::getsockname(pending.udp, reinterpret_cast<sockaddr*>(&address), &size) != 0)
      return failed("getsockname UDP");
    if (before_tcp_bind) before_tcp_bind(pending, address, attempt);
    if (::bind(pending.tcp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      failed("bind TCP");
      if (AddressInUse(error.code) && attempt < kPeerPortAttempts) continue;
      return false;
    }
    if (::listen(pending.tcp, 64) != 0) return failed("listen TCP");
    result.udp = std::exchange(pending.udp, kInvalidSocket);
    result.tcp = std::exchange(pending.tcp, kInvalidSocket);
    result.port = ntohs(address.sin_port);
    error.operation = "none"; error.code = 0;
    return true;
  }
  return false;
}
}  // namespace dns_fixture
