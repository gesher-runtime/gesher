// Independent WinSDK observation only: no Gesher descriptor or errno mapping.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool ok, const char* what) {
  if (!ok) { fprintf(stderr, "FAIL UDP oracle %s wsa=%d\n", what, WSAGetLastError()); exit(1); }
}
int main(int argc, char** argv) {
  require(argc == 4, "three exact binary arguments");
  for (int i = 1; i < 4; ++i)
    require(strlen(argv[i]) == 1 && argv[i][0] >= '0' && argv[i][0] <= (i == 2 ? '3' : '1'), "exact observation argument");
  int report_reset = argv[1][0] - '0', payload = argv[2][0] - '0', query_error = argv[3][0] - '0';
  const int payload_length = payload == 1 ? 1 : payload == 2 ? 0 : 8192;
  char expected[8192];
  for (int i = 0; i < 8192; ++i) expected[i] = static_cast<char>((i * 37) % 251 + 1);
  expected[0] = 'p';
  WSADATA data{};
  require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "startup");
  SOCKET target = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  SOCKET client = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  require(target != INVALID_SOCKET && client != INVALID_SOCKET, "owned endpoints");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(target, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "target bind");
  int length = sizeof(address);
  require(getsockname(target, reinterpret_cast<sockaddr*>(&address), &length) == 0, "target address");
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "client bind");
  length = sizeof(local);
  require(getsockname(client, reinterpret_cast<sockaddr*>(&local), &length) == 0, "client address");
  BOOL report = report_reset ? TRUE : FALSE; DWORD returned = 0;
  require(WSAIoctl(client, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr) == 0,
      "exact reset policy");
  u_long nonblocking = 1;
  require(ioctlsocket(client, FIONBIO, &nonblocking) == 0 &&
      connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connected nonblocking client");
  if (payload) {
    require(sendto(target, expected, payload_length, 0, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == payload_length, "real queued payload");
  } else {
    require(closesocket(target) == 0, "target physically closed"); target = INVALID_SOCKET;
    require(send(client, "u", 1, 0) == 1, "real ICMP-producing send");
  }
  WSAPOLLFD first{}; first.fd = client; first.events = POLLRDNORM | POLLRDBAND;
  int polled = WSAPoll(&first, 1, 2000), poll_error = polled == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("WINDOWS_UDP_RESET report=%d payload=%d query=%d stage=initial-poll rc=%d error=%d revents=%d payload_or_SO_ERROR_consumed=0\n",
      report_reset, payload, query_error, polled, poll_error, first.revents);
  if (query_error) {
    int value = -999, size = sizeof(value);
    int rc = getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &size);
    int failure = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    printf("WINDOWS_UDP_RESET report=%d payload=%d query=%d stage=explicit-SO_ERROR rc=%d error=%d value=%d may_consume_error=1\n",
        report_reset, payload, query_error, rc, failure, value);
  }
  WSAPOLLFD before{}; before.fd = client; before.events = first.events;
  int repeated = WSAPoll(&before, 1, 0), repeat_error = repeated == SOCKET_ERROR ? WSAGetLastError() : 0;
  char byte = '?'; int peeked = recv(client, &byte, 1, MSG_PEEK), peek_error = peeked == SOCKET_ERROR ? WSAGetLastError() : 0;
  int peek_byte = static_cast<unsigned char>(byte);
  printf("WINDOWS_UDP_RESET report=%d payload=%d query=%d stage=before-and-peek poll=%d poll_error=%d revents=%d recv=%d recv_error=%d byte=%d may_consume_error=1\n",
      report_reset, payload, query_error, repeated, repeat_error, before.revents, peeked, peek_error, peek_byte);
  WSAPOLLFD after{}; after.fd = client; after.events = first.events;
  int after_rc = WSAPoll(&after, 1, 0), after_error = after_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  char actual[8192]{};
  const int receive_capacity = payload <= 1 ? 1 : 8192;
  int consumed = recv(client, actual, receive_capacity, 0), receive_error = consumed == SOCKET_ERROR ? WSAGetLastError() : 0;
  bool conserved = payload && consumed == payload_length && !memcmp(expected, actual, static_cast<size_t>(payload_length));
  printf("WINDOWS_UDP_RESET report=%d payload=%d query=%d stage=after-peek-and-consume poll=%d poll_error=%d revents=%d recv=%d recv_error=%d expected_length=%d conserved=%d\n",
      report_reset, payload, query_error, after_rc, after_error, after.revents, consumed, receive_error, payload ? payload_length : -1, conserved ? 1 : 0);
  if (payload) require(polled == 1 && first.revents == POLLRDNORM && after_rc == 1 && after.revents == POLLRDNORM && conserved &&
      (payload_length > 1 || (peeked == payload_length && (payload_length == 0 || peek_byte == 'p'))),
      "entire real zero, one or large payload survives one-byte peek and explicit error query");
  WSAPOLLFD drained{}; drained.fd = client; drained.events = first.events;
  const int drained_rc = WSAPoll(&drained, 1, 0);
  const int drained_error = drained_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
  const int again = recv(client, actual, sizeof(actual), 0);
  const int again_error = again == SOCKET_ERROR ? WSAGetLastError() : 0;
  printf("WINDOWS_UDP_TOKEN report=%d payload=%d query=%d stage=after-ordinary-receive poll=%d poll_error=%d revents=%d recv=%d recv_error=%d\n",
      report_reset, payload, query_error, drained_rc, drained_error, drained.revents, again, again_error);
  if (payload) require(drained_rc == 0 && drained.revents == 0 && again == SOCKET_ERROR && again_error == WSAEWOULDBLOCK,
      "ordinary receive consumed exactly one datagram with no fabricated follow-up readiness");
  if (!payload) {
    require(send(client, "v", 1, 0) == 1, "new independent ICMP-producing send after ordinary consumption");
    WSAPOLLFD next{}; next.fd = client; next.events = first.events;
    const int next_rc = WSAPoll(&next, 1, 2000);
    const int next_error = next_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    const int next_peek = recv(client, actual, sizeof(actual), MSG_PEEK);
    const int next_peek_error = next_peek == SOCKET_ERROR ? WSAGetLastError() : 0;
    const int next_recv = recv(client, actual, sizeof(actual), 0);
    const int next_recv_error = next_recv == SOCKET_ERROR ? WSAGetLastError() : 0;
    WSAPOLLFD final{}; final.fd = client; final.events = first.events;
    const int final_rc = WSAPoll(&final, 1, 0);
    const int final_error = final_rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    printf("WINDOWS_UDP_TOKEN report=%d payload=0 query=%d stage=new-send poll=%d poll_error=%d revents=%d peek=%d peek_error=%d recv=%d recv_error=%d final_poll=%d final_poll_error=%d final_revents=%d\n",
        report_reset, query_error, next_rc, next_error, next.revents, next_peek, next_peek_error,
        next_recv, next_recv_error, final_rc, final_error, final.revents);
  }
  bool target_closed = target == INVALID_SOCKET || closesocket(target) == 0;
  bool client_closed = closesocket(client) == 0;
  require(target_closed && client_closed, "both owned endpoints closed");
  require(WSACleanup() == 0, "context cleanup");
  puts("PASS Windows UDP reset characterization: raw errors and flags are observations only");
  return 0;
}
