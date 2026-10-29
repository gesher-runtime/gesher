// Independent Linux-only observation of urgent sequence consumption and close.
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL LINUX_ALIAS_CLOSE %s errno=%d\n", label, errno); exit(1); }
}
static long long millis() {
  timespec value{}; require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "monotonic clock");
  return static_cast<long long>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}
struct Pair { int sender, receiver; };
static Pair pair() {
  int listener = socket(AF_INET, SOCK_STREAM, 0); require(listener >= 0, "listener");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "real listener");
  socklen_t length = sizeof(address); require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "listener address");
  int sender = socket(AF_INET, SOCK_STREAM, 0); require(sender >= 0 && connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "real client");
  int receiver = accept(listener, nullptr, nullptr); require(receiver >= 0 && close(listener) == 0, "accepted transport");
  return {sender, receiver};
}
static void scenario(int mode) {
  Pair a = pair(); const long long start = millis();
  if (mode == 3) {
    const int sibling = dup(a.receiver); require(sibling >= 0 && close(a.receiver) == 0, "surviving actual alias"); a.receiver = sibling;
  }
  linger policy{}; socklen_t length = sizeof(policy);
  require(getsockopt(a.receiver, SOL_SOCKET, SO_LINGER, &policy, &length) == 0 && length == sizeof(policy), "actual unchanged linger policy");
  require(policy.l_onoff == 0, "no abortive-close option introduced");
  require(send(a.sender, mode ? "!" : "n", 1, mode ? MSG_OOB : 0) == 1, "exact single-byte input");
  pollfd ready{a.receiver, static_cast<short>(mode ? POLLPRI : POLLIN), 0};
  require(poll(&ready, 1, 2000) == 1 && (ready.revents & ready.events), "actual byte arrived");
  char byte{};
  require(recv(a.receiver, &byte, 1, mode ? MSG_OOB : 0) == 1 && byte == (mode ? '!' : 'n'), "caller consumes exact input byte");
  int drained = -99, drain_error = -99, live_rc = -99, live_error = -99;
  if (mode >= 2) {
    require(shutdown(a.sender, SHUT_WR) == 0, "sender half-close gives finite stream drain");
    pollfd stream{a.receiver, POLLIN, 0}; require(poll(&stream, 1, 2000) == 1, "actual stream FIN arrived");
    drained = static_cast<int>(recv(a.receiver, &byte, 1, MSG_DONTWAIT)); drain_error = drained < 0 ? errno : 0;
    require(drained == 0, "normal receive advances through urgent sequence to real FIN");
    live_rc = static_cast<int>(recv(a.sender, &byte, 1, MSG_DONTWAIT)); live_error = live_rc < 0 ? errno : 0;
    require(live_rc == -1 && (live_error == EAGAIN || live_error == EWOULDBLOCK), "peer has no FIN while last receiving alias is still live");
  }
  require(close(a.receiver) == 0, "actual last receiver close");
  pollfd peer{a.sender, POLLIN | POLLERR | POLLHUP, 0};
  const int polled = poll(&peer, 1, 2000);
  const int result = polled == 1 ? static_cast<int>(recv(a.sender, &byte, 1, MSG_DONTWAIT)) : -99;
  const int error = result < 0 ? errno : 0;
  printf("LINUX_ALIAS_CLOSE mode=%d urgent=%d alias=%d linger=%d stream_drain=%d drain_error=%d before_last_close_rc=%d before_last_close_error=%d poll=%d revents=%d recv=%d error=%d elapsed_ms=%lld\n",
      mode, mode != 0, mode == 3, policy.l_onoff, drained, drain_error, live_rc, live_error, polled, peer.revents, result, error, millis() - start); fflush(stdout);
  require(polled == 1, "actual finite peer close notification");
  if (mode != 1) require(result == 0, "strict EOF after ordinary consumption or observed stream drain");
  // Mode1 is an observation cell: no SO_ERROR/extra read changes its ordering.
  require(close(a.sender) == 0, "peer cleanup");
}
int main() {
  for (int mode = 0; mode != 4; ++mode) scenario(mode);
  puts("PASS independent Linux ordinary and urgent close observations with strict drained last-alias EOF controls");
}
