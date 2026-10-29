// Independent POSIX lifecycle reference for the overlapped-connect investigation.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void require(bool ok, const char* label) {
  if (!ok) { fprintf(stderr, "FAIL LINUX_CONNECT %s errno=%d\n", label, errno); exit(1); }
}
static int option(int fd, int level, int name) {
  int value = -1; socklen_t length = sizeof(value);
  require(getsockopt(fd, level, name, &value, &length) == 0 && length == sizeof(value), "option read"); return value;
}
static void scenario(bool success, bool nonblocking, bool flip) {
  int target = socket(AF_INET, SOCK_STREAM, 0); require(target >= 0, "target");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(target, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "target bind");
  socklen_t length = sizeof(address);
  require(getsockname(target, reinterpret_cast<sockaddr*>(&address), &length) == 0, "target name");
  if (success) require(listen(target, 1) == 0, "target listen");
  int client = socket(AF_INET, SOCK_STREAM | (nonblocking ? SOCK_NONBLOCK : 0), 0); require(client >= 0, "client");
  sockaddr_in local{}; local.sin_family = AF_INET;
  require(bind(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "explicit local bind");
  length = sizeof(local); require(getsockname(client, reinterpret_cast<sockaddr*>(&local), &length) == 0, "initial local address");
  const unsigned short original_port = local.sin_port;
  int on = 1; timeval timeout{0, 300000};
  require(setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0 &&
          setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0 &&
          setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "explicit properties");
  const int connected = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)); const int connect_error = errno;
  if (flip) {
    int flags = fcntl(client, F_GETFL); require(flags >= 0, "current mode");
    require(fcntl(client, F_SETFL, nonblocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == 0, "mode change after call");
  }
  int first_error;
  if (connected == -1 && connect_error == EINPROGRESS) {
    pollfd p{client, POLLOUT, 0}; require(poll(&p, 1, 2000) == 1, "pending completion");
    first_error = option(client, SOL_SOCKET, SO_ERROR);
    require(first_error == (success ? 0 : ECONNREFUSED), "nonblocking completion result");
  } else {
    require(success ? connected == 0 : (connected == -1 && connect_error == ECONNREFUSED), "synchronous completion result");
    first_error = option(client, SOL_SOCKET, SO_ERROR);
    require(first_error == 0, "synchronous caller already consumed connect error");
  }
  const int second_error = option(client, SOL_SOCKET, SO_ERROR); require(second_error == 0, "caller error read consumes once");
  length = sizeof(local); require(getsockname(client, reinterpret_cast<sockaddr*>(&local), &length) == 0 && local.sin_port == original_port, "explicit local port retained");
  require(option(client, SOL_SOCKET, SO_KEEPALIVE) == 1 && option(client, IPPROTO_TCP, TCP_NODELAY) == 1, "properties retained");
  length = sizeof(timeout); require(getsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, &length) == 0 && timeout.tv_sec == 0 && timeout.tv_usec == 300000, "receive timeout retained");
  int mode = fcntl(client, F_GETFL); require(mode >= 0 && ((mode & O_NONBLOCK) != 0) == (nonblocking != flip), "actual final mode");
  printf("LINUX_CONNECT success=%d initial_nonblock=%d flip=%d connect_rc=%d connect_errno=%d first_error=%d second_error=%d local_port=%u\n", success ? 1 : 0, nonblocking ? 1 : 0, flip ? 1 : 0, connected, connect_error, first_error, second_error, ntohs(local.sin_port)); fflush(stdout);
  if (success) {
    sockaddr_in peer{}; length = sizeof(peer);
    require(getpeername(client, reinterpret_cast<sockaddr*>(&peer), &length) == 0 && peer.sin_port == address.sin_port, "actual peer");
    int accepted = accept(target, nullptr, nullptr); require(accepted >= 0, "accepted peer");
    require(send(accepted, "N", 1, 0) == 1, "normal send");
    pollfd p{client, POLLIN, 0}; require(poll(&p, 1, 2000) == 1, "normal arrival"); char byte = 0;
    require(recv(client, &byte, 1, 0) == 1 && byte == 'N', "normal payload");
    require(send(accepted, "!", 1, MSG_OOB) == 1, "urgent send");
    p = {client, POLLPRI, 0}; require(poll(&p, 1, 2000) == 1 && p.revents == POLLPRI, "urgent arrival");
    require(recv(client, &byte, 1, MSG_OOB) == 1 && byte == '!', "urgent payload");
    p.revents = 0; require(poll(&p, 1, 0) == 0 && p.revents == 0, "urgent cleared");
    require(close(accepted) == 0, "accepted close");
  }
  require(close(client) == 0 && close(target) == 0, "endpoint cleanup");
}
int main() {
  for (int success = 0; success != 2; ++success) for (int mode = 0; mode != 2; ++mode) for (int flip = 0; flip != 2; ++flip) scenario(success != 0, mode != 0, flip != 0);
  puts("PASS independent Linux connect mode/bind/options/error/urgent lifecycle reference");
}
