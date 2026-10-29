// Independent WinSDK characterization; never links Gesher or consumes SO_ERROR.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void require(bool ok, const char* what) {
  if (!ok) { fprintf(stderr, "FAIL %s wsa=%d\n", what, WSAGetLastError()); exit(1); }
}
static void wait_bytes(SOCKET s, unsigned long long bytes) {
  const ULONGLONG deadline = GetTickCount64() + 2000;
  for (;;) {
    TCP_INFO_v0 info{}; ULONG version = 0; DWORD returned = 0;
    require(WSAIoctl(s, SIO_TCP_INFO, &version, sizeof(version), &info, sizeof(info), &returned, nullptr, nullptr) == 0, "TCP_INFO receive proof");
    if (info.BytesIn >= bytes) return;
    require(GetTickCount64() < deadline, "actual TCP receive deadline");
    Sleep(1);
  }
}
static void receive_prefix(SOCKET s) {
  char prefix[6]{};
  int done = 0;
  while (done < 6) {
    int n = recv(s, prefix + done, 6 - done, 0);
    require(n > 0, "prefix consume");
    done += n;
  }
  require(memcmp(prefix, "normal", 6) == 0, "prefix bytes preserved");
}
static void snapshot(SOCKET s, const char* phase, int inline_oob) {
  fd_set reads, exceptions;
  FD_ZERO(&reads); FD_ZERO(&exceptions); FD_SET(s, &reads); FD_SET(s, &exceptions);
  timeval zero{};
  int n = select(0, &reads, nullptr, &exceptions, &zero);
  require(n >= 0, "select");
  WSAPOLLFD band{}; band.fd = s; band.events = POLLRDBAND;
  int band_rc = WSAPoll(&band, 1, 0);
  int band_error = band_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  u_long mark = 99, available = 99;
  int m = ioctlsocket(s, SIOCATMARK, &mark);
  int mark_error = m == SOCKET_ERROR ? WSAGetLastError() : 0;
  require(ioctlsocket(s, FIONREAD, &available) == 0, "available");
  printf("WINDOWS_URG inline=%d phase=%s read=%d except=%d mark_rc=%d mark_error=%d mark=%lu bytes=%lu band_rc=%d band_error=%d band_revents=%d\n",
         inline_oob, phase, FD_ISSET(s, &reads) != 0, FD_ISSET(s, &exceptions) != 0,
         m, mark_error, mark, available, band_rc, band_error, band.revents);
  fflush(stdout);
}
int main() {
  WSADATA data{};
  require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
  for (int inline_oob = 0; inline_oob <= 3; ++inline_oob) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "listener");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
    int length = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "name");
    require(listen(listener, 1) == 0, "listen");
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
    SOCKET server = accept(listener, nullptr, nullptr);
    require(server != INVALID_SOCKET, "accept");
    int host_inline = inline_oob == 1 ? 1 : 0;
    require(setsockopt(server, SOL_SOCKET, SO_OOBINLINE, reinterpret_cast<const char*>(&host_inline), sizeof(host_inline)) == 0, "inline");
    DWORD timeout = 2000;
    require(setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "timeout");
    snapshot(server, "empty", inline_oob);
    require(send(client, "normal", 6, 0) == 6, "normal send");
    wait_bytes(server, 6);
    snapshot(server, "normal", inline_oob);
    require(send(client, "!", 1, MSG_OOB) == 1, "urgent send");
    wait_bytes(server, 7);
    snapshot(server, "prefix-and-urgent", inline_oob);
    if (inline_oob >= 2) {
      require(send(client, "tail", 4, 0) == 4, "tail send");
      wait_bytes(server, 11);
      snapshot(server, "prefix-urgent-tail", inline_oob);
      if (inline_oob == 3) {
        char big[32]{};
        int peeked = recv(server, big, sizeof(big), MSG_PEEK);
        printf("WINDOWS_BOUNDARY phase=big-normal-peek rc=%d error=%d\n", peeked, peeked < 0 ? WSAGetLastError() : 0);
        require(peeked == 6 && memcmp(big, "normal", 6) == 0, "BIG peek stops before urgent and preserves prefix");
        snapshot(server, "after-big-prefix-peek", inline_oob);
        int received = recv(server, big, sizeof(big), 0);
        printf("WINDOWS_BOUNDARY phase=big-normal-read rc=%d error=%d\n", received, received < 0 ? WSAGetLastError() : 0);
        require(received == 6 && memcmp(big, "normal", 6) == 0, "BIG read stops before urgent");
      } else {
        receive_prefix(server);
      }
      snapshot(server, "prefix-consumed-urgent-tail-remain", inline_oob);
      if (inline_oob == 3) {
        char byte{};
        int urgent_peek = recv(server, &byte, 1, MSG_OOB | MSG_PEEK);
        printf("WINDOWS_BOUNDARY phase=atmark-oob-peek rc=%d error=%d byte=%d\n", urgent_peek, urgent_peek < 0 ? WSAGetLastError() : 0, static_cast<int>(byte));
        require(urgent_peek == 1 && byte == '!', "OOB peek at boundary preserves byte");
        char tail_peek[32]{};
        int normal_peek = recv(server, tail_peek, sizeof(tail_peek), MSG_PEEK);
        printf("WINDOWS_BOUNDARY phase=atmark-normal-peek rc=%d error=%d\n", normal_peek, normal_peek < 0 ? WSAGetLastError() : 0);
        require(normal_peek == 4 && memcmp(tail_peek, "tail", 4) == 0, "normal PEEK at mark observes suffix without consuming urgent");
        snapshot(server, "after-atmark-normal-peek", inline_oob);
      }
      char urgent{};
      require(recv(server, &urgent, 1, MSG_OOB) == 1 && urgent == '!', "post-prefix urgent consume");
      snapshot(server, "urgent-consumed-tail-remains", inline_oob);
      char tail[4]{}; int got = 0;
      while (got < 4) { int n = recv(server, tail + got, 4 - got, 0); require(n > 0, "tail progress"); got += n; }
      require(memcmp(tail, "tail", 4) == 0, "tail preserved");
      require(closesocket(server) == 0 && closesocket(client) == 0 && closesocket(listener) == 0, "boundary cleanup");
      continue;
    }
    char bytes[16]{};
    if (!inline_oob) {
      require(recv(server, bytes, 1, MSG_OOB) == 1 && bytes[0] == '!', "urgent consume");
      snapshot(server, "urgent-consumed-prefix-remains", inline_oob);
    }
    receive_prefix(server);
    snapshot(server, "prefix-consumed", inline_oob);
    if (inline_oob) {
      require(recv(server, bytes, 1, 0) == 1 && bytes[0] == '!', "inline urgent consume");
      snapshot(server, "urgent-consumed", inline_oob);
    }
    require(closesocket(server) == 0 && closesocket(client) == 0 && closesocket(listener) == 0, "close");
  }
  // A refused nonblocking connection is exception-ready, but is not urgent.
  SOCKET reserved = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  require(reserved != INVALID_SOCKET, "refused reserve");
  sockaddr_in refused{}; refused.sin_family = AF_INET; refused.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(reserved, reinterpret_cast<sockaddr*>(&refused), sizeof(refused)) == 0, "refused bind");
  int length = sizeof(refused);
  require(getsockname(reserved, reinterpret_cast<sockaddr*>(&refused), &length) == 0, "refused address");
  SOCKET failed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  require(failed != INVALID_SOCKET, "refused socket");
  u_long nonblocking = 1; require(ioctlsocket(failed, FIONBIO, &nonblocking) == 0, "refused nonblocking");
  int connection = connect(failed, reinterpret_cast<sockaddr*>(&refused), sizeof(refused));
  require(connection == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "refused connect pending");
  fd_set exceptions; FD_ZERO(&exceptions); FD_SET(failed, &exceptions); timeval wait{5, 0};
  require(select(0, nullptr, nullptr, &exceptions, &wait) == 1, "refused exception arrived");
  sockaddr_storage peer{}; length = sizeof(peer);
  int peer_rc = getpeername(failed, reinterpret_cast<sockaddr*>(&peer), &length);
  int peer_error = peer_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("WINDOWS_URG refused-connect except=1 peer_rc=%d peer_error=%d SO_ERROR=untouched\n", peer_rc, peer_error);
  require(peer_rc == SOCKET_ERROR && peer_error == WSAENOTCONN, "refused is not connected");
  require(closesocket(failed) == 0 && closesocket(reserved) == 0, "refused cleanup");
  require(WSACleanup() == 0, "cleanup");
  puts("PASS Windows TCP urgent characterization");
}
