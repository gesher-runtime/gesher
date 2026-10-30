// Independent Linux differential: connected UDP error and peek conservation.
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void require(bool ok, const char* text) {
  if (!ok) { fprintf(stderr, "FAIL Linux UDP reference %s errno=%d\n", text, errno); exit(1); }
}
int main(int argc, char** argv) {
  require(argc == 4 && strlen(argv[1]) == 1 && (argv[1][0] == '4' || argv[1][0] == '6') &&
      strlen(argv[2]) == 1 && argv[2][0] >= '0' && argv[2][0] <= '3' &&
      strlen(argv[3]) == 1 && (argv[3][0] == '0' || argv[3][0] == '1'), "exact arguments");
  int version = argv[1][0] - '0', mode = argv[2][0] - '0', query = argv[3][0] - '0';
  int family = version == 4 ? AF_INET : AF_INET6;
  int target = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0), client = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  require(target >= 0 && client >= 0, "owned native endpoints");
  sockaddr_storage address{}, local{};
  socklen_t size;
  if (version == 4) {
    auto* a = reinterpret_cast<sockaddr_in*>(&address); a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto* b = reinterpret_cast<sockaddr_in*>(&local); b->sin_family = AF_INET; b->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    size = sizeof(sockaddr_in);
  } else {
    auto* a = reinterpret_cast<sockaddr_in6*>(&address); a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback;
    auto* b = reinterpret_cast<sockaddr_in6*>(&local); b->sin6_family = AF_INET6; b->sin6_addr = in6addr_loopback;
    size = sizeof(sockaddr_in6);
  }
  require(bind(target, reinterpret_cast<sockaddr*>(&address), size) == 0 && bind(client, reinterpret_cast<sockaddr*>(&local), size) == 0, "loopback binds");
  socklen_t address_size = size, local_size = size;
  require(getsockname(target, reinterpret_cast<sockaddr*>(&address), &address_size) == 0 &&
      getsockname(client, reinterpret_cast<sockaddr*>(&local), &local_size) == 0 &&
      connect(client, reinterpret_cast<sockaddr*>(&address), address_size) == 0, "actual connected endpoints");
  int payload_length = mode == 1 ? 1 : mode == 2 ? 0 : 8192;
  char expected[8192]; for (int i = 0; i < 8192; ++i) expected[i] = static_cast<char>((i * 37) % 251 + 1); expected[0] = 'p';
  if (mode) require(sendto(target, expected, payload_length, 0, reinterpret_cast<sockaddr*>(&local), local_size) == payload_length, "real queued datagram");
  else { require(close(target) == 0, "closed actual peer"); target = -1; require(send(client, "u", 1, 0) == 1, "ICMP-producing send"); }
  pollfd initial{client, POLLIN | POLLPRI, 0}; int polled = poll(&initial, 1, 2000);
  printf("LINUX_UDP_REFERENCE family=%d mode=%d query=%d stage=initial-poll rc=%d revents=%d payload_or_SO_ERROR_consumed=0\n", version, mode, query, polled, initial.revents);
  require(polled == 1 && (mode ? initial.revents == POLLIN : (initial.revents & POLLERR) && !(initial.revents & POLLPRI)), "Linux payload/error readiness");
  if (query) {
    int value = -999; socklen_t value_size = sizeof(value); int rc = getsockopt(client, SOL_SOCKET, SO_ERROR, &value, &value_size);
    printf("LINUX_UDP_REFERENCE family=%d mode=%d query=%d stage=explicit-SO_ERROR rc=%d value=%d\n", version, mode, query, rc, value);
    require(rc == 0 && value == (mode ? 0 : ECONNREFUSED), "exact caller error token");
  }
  pollfd before{client, POLLIN | POLLPRI, 0}; int repeated = poll(&before, 1, 0);
  char byte{}; int peeked = static_cast<int>(recv(client, &byte, 1, MSG_PEEK)), peek_error = peeked < 0 ? errno : 0;
  pollfd after{client, POLLIN | POLLPRI, 0}; int after_rc = poll(&after, 1, 0);
  char actual[8192]{}; int received = static_cast<int>(recv(client, actual, sizeof(actual), 0)), receive_error = received < 0 ? errno : 0;
  bool conserved = mode && received == payload_length && !memcmp(expected, actual, static_cast<size_t>(payload_length));
  printf("LINUX_UDP_REFERENCE family=%d mode=%d query=%d stage=peek-and-consume before_poll=%d before_mask=%d peek=%d peek_error=%d after_poll=%d after_mask=%d recv=%d recv_error=%d expected_length=%d conserved=%d\n",
      version, mode, query, repeated, before.revents, peeked, peek_error, after_rc, after.revents, received, receive_error, mode ? payload_length : -1, conserved ? 1 : 0);
  if (mode) require(conserved && peeked == (payload_length ? 1 : 0) && after_rc == 1 && after.revents == POLLIN, "entire datagram conserved after peek/error query");
  else require(peeked == -1 && peek_error == (query ? EAGAIN : ECONNREFUSED) && after_rc == 0 && received == -1 && receive_error == EAGAIN, "Linux pending error consumption remains explicit");
  if (!mode) {
    require(send(client, "v", 1, 0) == 1, "new ICMP-producing send after consumed token");
    pollfd next{client, POLLIN | POLLPRI, 0};
    const int next_rc = poll(&next, 1, 2000);
    const int next_recv = static_cast<int>(recv(client, actual, sizeof(actual), 0));
    const int next_error = next_recv < 0 ? errno : 0;
    pollfd final{client, POLLIN | POLLPRI, 0};
    const int final_rc = poll(&final, 1, 0);
    printf("LINUX_UDP_TOKEN family=%d query=%d stage=new-send poll=%d revents=%d recv=%d recv_error=%d final_poll=%d final_revents=%d\n",
        version, query, next_rc, next.revents, next_recv, next_error, final_rc, final.revents);
    require(next_rc == 1 && next.revents == POLLERR && next_recv == -1 && next_error == ECONNREFUSED &&
        final_rc == 0 && final.revents == 0, "new error generation remains observable and consumed once");
  }
  bool target_closed = target == -1 || close(target) == 0, client_closed = close(client) == 0;
  require(target_closed && client_closed, "all owned endpoints closed");
  puts("PASS Linux UDP reference connected error token and exact payload conservation");
}
