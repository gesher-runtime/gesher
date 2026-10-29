// Independent WinSDK characterization; no Gesher. Error ownership controls
// read SO_ERROR explicitly only after the named observation sequence completes.
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
static SOCKET duplicate(SOCKET s) {
  WSAPROTOCOL_INFOW info{};
  require(WSADuplicateSocketW(s, GetCurrentProcessId(), &info) == 0, "ownership duplicate info");
  SOCKET copy = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(copy != INVALID_SOCKET, "ownership duplicate socket");
  u_long nonblocking = 1; require(ioctlsocket(copy, FIONBIO, &nonblocking) == 0, "ownership duplicate nonblocking");
  return copy;
}
static void tcp_state(SOCKET s, int mode, const char* phase) {
  TCP_INFO_v0 info{}; ULONG version = 0; DWORD returned = 0;
  int rc = WSAIoctl(s, SIO_TCP_INFO, &version, sizeof(version), &info, sizeof(info), &returned, nullptr, nullptr);
  int error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  WSAPROTOCOL_INFOW protocol{}; int length = sizeof(protocol);
  int protocol_rc = getsockopt(s, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocol), &length);
  printf("WINDOWS_ERROR_STATE mode=%d phase=%s tcp_rc=%d tcp_error=%d returned=%lu state=%d closed=%d protocol_rc=%d family=%d type=%d protocol=%d\n",
         mode, phase, rc, error, returned, static_cast<int>(info.State), static_cast<int>(TCPSTATE_CLOSED),
         protocol_rc, protocol.iAddressFamily, protocol.iSocketType, protocol.iProtocol);
  fflush(stdout);
}
static void error_ownership(int mode) {
  SOCKET reserved = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  require(reserved != INVALID_SOCKET, "ownership reserved");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(reserved, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "ownership bind");
  int length = sizeof(address);
  require(getsockname(reserved, reinterpret_cast<sockaddr*>(&address), &length) == 0, "ownership address");
  SOCKET original = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  require(original != INVALID_SOCKET, "ownership socket");
  u_long nonblocking = 1; require(ioctlsocket(original, FIONBIO, &nonblocking) == 0, "ownership nonblocking");
  SOCKET connecting = (mode == 3 || mode == 5) ? duplicate(original) : original;
  require(connect(connecting, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "ownership connect pending");
  if (connecting != original) require(closesocket(connecting) == 0, "ownership close connecting alias");
  const ULONGLONG start = GetTickCount64();
  if (mode == 0) {
    fd_set exceptions; FD_ZERO(&exceptions); FD_SET(original, &exceptions); timeval wait{5, 0};
    require(select(0, nullptr, nullptr, &exceptions, &wait) == 1, "ownership select refusal");
  } else {
    if (mode == 5) {
      int value = 0; length = sizeof(value);
      require(getsockopt(original, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&value), &length) == 0, "ownership type");
      length = sizeof(value);
      require(getsockopt(original, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&value), &length) == 0, "ownership listener");
      sockaddr_storage peer{}; length = sizeof(peer);
      int rc = getpeername(original, reinterpret_cast<sockaddr*>(&peer), &length);
      printf("WINDOWS_ERROR mode=%d phase=pre-peer rc=%d error=%d\n", mode, rc, rc == SOCKET_ERROR ? WSAGetLastError() : 0);
    }
    WSAPOLLFD p{}; p.fd = original; p.events = POLLRDBAND | POLLWRNORM;
    int rc = WSAPoll(&p, 1, 5000);
    printf("WINDOWS_ERROR mode=%d phase=poll rc=%d revents=%d error=%d elapsed_ms=%llu\n", mode, rc, p.revents, rc == SOCKET_ERROR ? WSAGetLastError() : 0, GetTickCount64() - start);
    require(rc == 1, "ownership poll refusal");
  }
  if (mode == 2 || mode == 5) {
    sockaddr_storage peer{}; length = sizeof(peer);
    int rc = getpeername(original, reinterpret_cast<sockaddr*>(&peer), &length);
    printf("WINDOWS_ERROR mode=%d phase=post-peer rc=%d error=%d\n", mode, rc, rc == SOCKET_ERROR ? WSAGetLastError() : 0);
  }
  if (mode == 4 || mode == 5) {
    SOCKET alias = duplicate(original);
    WSAPOLLFD p{}; p.fd = alias; p.events = POLLRDBAND | POLLWRNORM;
    int rc = WSAPoll(&p, 1, 0);
    printf("WINDOWS_ERROR mode=%d phase=alias-poll rc=%d revents=%d error=%d\n", mode, rc, p.revents, rc == SOCKET_ERROR ? WSAGetLastError() : 0);
    require(closesocket(alias) == 0, "ownership late alias close");
  }
  int first = -1; length = sizeof(first);
  tcp_state(original, mode, "before-caller");
  int first_rc = getsockopt(original, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&first), &length);
  int first_error = first_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  int second = -1; length = sizeof(second);
  int second_rc = getsockopt(original, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&second), &length);
  printf("WINDOWS_ERROR mode=%d phase=caller-consumes first_rc=%d value=%d error=%d second_rc=%d second=%d elapsed_ms=%llu\n", mode, first_rc, first, first_error, second_rc, second, GetTickCount64() - start);
  fflush(stdout);
  require(first_rc == 0 && second_rc == 0, "ownership explicit caller getsockopt");
  tcp_state(original, mode, "after-caller");
  int sent = send(original, "x", 1, 0);
  int send_error = sent == SOCKET_ERROR ? WSAGetLastError() : 0;
  int after_send = -1; length = sizeof(after_send);
  int after_send_rc = getsockopt(original, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&after_send), &length);
  printf("WINDOWS_ERROR_STATE mode=%d phase=after-send send_rc=%d send_error=%d getter_rc=%d value=%d\n", mode, sent, send_error, after_send_rc, after_send);
  tcp_state(original, mode, "after-send");
  // Reconnect after failed TCP is unspecified. Observe the actual provider;
  // no cross-platform reconnect success is asserted or claimed.
  int retry = connect(original, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  int retry_error = retry == SOCKET_ERROR ? WSAGetLastError() : 0;
  tcp_state(original, mode, "after-retry");
  WSAPOLLFD retry_poll{}; retry_poll.fd = original; retry_poll.events = POLLRDBAND | POLLWRNORM;
  int retry_poll_rc = WSAPoll(&retry_poll, 1, 5000);
  int retry_value = -1; length = sizeof(retry_value);
  int retry_getter = getsockopt(original, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&retry_value), &length);
  printf("WINDOWS_ERROR_STATE mode=%d phase=retry-result connect_rc=%d connect_error=%d poll_rc=%d revents=%d getter_rc=%d value=%d\n", mode, retry, retry_error, retry_poll_rc, retry_poll.revents, retry_getter, retry_value);
  tcp_state(original, mode, "retry-complete");
  require(closesocket(original) == 0 && closesocket(reserved) == 0, "ownership cleanup");
}
int main() {
  WSADATA data{};
  require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
  SOCKET fresh = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  require(fresh != INVALID_SOCKET, "fresh stream");
  WSAPOLLFD initial{}; initial.fd = fresh; initial.events = POLLRDBAND | POLLWRNORM;
  int initial_rc = WSAPoll(&initial, 1, 0);
  printf("WINDOWS_URG fresh-stream events=%d rc=%d revents=%d SO_ERROR=not-read\n", initial.events, initial_rc, initial.revents);
  require(closesocket(fresh) == 0, "fresh stream cleanup");
  for (int inline_oob = 0; inline_oob <= 4; ++inline_oob) {
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
      if (inline_oob >= 3) {
        char big[32]{};
        int peeked = recv(server, big, sizeof(big), MSG_PEEK);
        printf("WINDOWS_BOUNDARY phase=big-normal-peek rc=%d error=%d\n", peeked, peeked < 0 ? WSAGetLastError() : 0);
        require(peeked == 6 && memcmp(big, "normal", 6) == 0, "BIG peek stops before urgent and preserves prefix");
        snapshot(server, "after-big-prefix-peek", inline_oob);
        int received = recv(server, big, inline_oob == 3 ? static_cast<int>(sizeof(big)) : peeked, 0);
        printf("WINDOWS_BOUNDARY phase=big-normal-read rc=%d error=%d\n", received, received < 0 ? WSAGetLastError() : 0);
        if (inline_oob == 3) {
          // Retain the disproved hypothesis as a precise native counterexample:
          // run408528 returned ten bytes, unlike the six-byte preceding peek.
          require(received == 10 && memcmp(big, "normaltail", 10) == 0, "native BIG read crosses urgent position as independently observed");
          snapshot(server, "after-crossing-big-read", inline_oob);
          char urgent{};
          int pending = recv(server, &urgent, 1, MSG_OOB | MSG_PEEK);
          printf("WINDOWS_BOUNDARY phase=after-crossing-oob-peek rc=%d error=%d byte=%d\n", pending, pending < 0 ? WSAGetLastError() : 0, static_cast<int>(urgent));
          require(closesocket(server) == 0 && closesocket(client) == 0 && closesocket(listener) == 0, "crossing case cleanup");
          continue;
        }
        require(received == 6 && memcmp(big, "normal", 6) == 0, "capped normal read preserves prefix");
      } else {
        receive_prefix(server);
      }
      snapshot(server, "prefix-consumed-urgent-tail-remain", inline_oob);
      if (inline_oob == 4) {
        char byte{};
        int urgent_peek = recv(server, &byte, 1, MSG_OOB | MSG_PEEK);
        printf("WINDOWS_BOUNDARY phase=atmark-oob-peek rc=%d error=%d byte=%d\n", urgent_peek, urgent_peek < 0 ? WSAGetLastError() : 0, static_cast<int>(byte));
        require(urgent_peek == 1 && byte == '!', "OOB peek at boundary preserves byte");
        char tail_peek[32]{};
        int normal_peek = recv(server, tail_peek, sizeof(tail_peek), MSG_PEEK);
        printf("WINDOWS_BOUNDARY phase=atmark-normal-peek rc=%d error=%d\n", normal_peek, normal_peek < 0 ? WSAGetLastError() : 0);
        // The return at the mark is the next independent observation. Do not
        // infer suffix visibility or a Linux-style position from SIOCATMARK.
        require(normal_peek >= 0, "normal PEEK at mark bounded completion");
        if (normal_peek > 0) require(normal_peek == 4 && memcmp(tail_peek, "tail", 4) == 0, "peeked bytes are exact suffix");
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
  const ULONGLONG poll_start = GetTickCount64();
  WSAPOLLFD refused_band{}; refused_band.fd = failed; refused_band.events = POLLRDBAND;
  int refused_rc = WSAPoll(&refused_band, 1, 5000);
  int refused_error = refused_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("WINDOWS_URG refused-band rc=%d revents=%d error=%d elapsed_ms=%llu SO_ERROR=not-read\n",
         refused_rc, refused_band.revents, refused_error, GetTickCount64() - poll_start);
  fflush(stdout);
  fd_set exceptions; FD_ZERO(&exceptions); FD_SET(failed, &exceptions); timeval wait{5, 0};
  require(select(0, nullptr, nullptr, &exceptions, &wait) == 1, "refused exception arrived");
  sockaddr_storage peer{}; length = sizeof(peer);
  int peer_rc = getpeername(failed, reinterpret_cast<sockaddr*>(&peer), &length);
  int peer_error = peer_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("WINDOWS_URG refused-connect except=1 peer_rc=%d peer_error=%d SO_ERROR=not-read\n", peer_rc, peer_error);
  require(peer_rc == SOCKET_ERROR && peer_error == WSAENOTCONN, "refused is not connected");
  const SHORT refused_interests[] = {POLLRDBAND, POLLRDBAND | POLLRDNORM, POLLRDBAND | POLLWRNORM, POLLRDBAND | POLLRDNORM | POLLWRNORM};
  for (SHORT events : refused_interests) {
    WSAPOLLFD observed{}; observed.fd = failed; observed.events = events;
    int rc = WSAPoll(&observed, 1, 0);
    int error = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    printf("WINDOWS_URG refused-complete events=%d rc=%d revents=%d error=%d SO_ERROR=not-read\n", events, rc, observed.revents, error);
  }
  require(closesocket(failed) == 0 && closesocket(reserved) == 0, "refused cleanup");
  for (int mode = 0; mode < 6; ++mode) error_ownership(mode);
  require(WSACleanup() == 0, "cleanup");
  puts("PASS Windows TCP urgent characterization");
}
