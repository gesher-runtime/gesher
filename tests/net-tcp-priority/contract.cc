// The identical contract is compiled against Linux, Gesher host POSIX, and both NDK ABIs.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void require(bool ok, const char* what) {
  if (!ok) { fprintf(stderr, "FAIL TCP priority: %s errno=%d\n", what, errno); exit(1); }
}
struct Pair { int sender, receiver; };
static Pair pair() {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  require(listener >= 0, "listener");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
  socklen_t length = sizeof(address);
  require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "address");
  require(listen(listener, 1) == 0, "listen");
  int sender = socket(AF_INET, SOCK_STREAM, 0);
  require(sender >= 0 && connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
  int receiver = accept(listener, nullptr, nullptr);
  require(receiver >= 0, "accept");
  timeval timeout{2, 0};
  require(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "receive deadline");
  require(close(listener) == 0, "listener close");
  return {sender, receiver};
}
static void ready(int fd, bool urgent, int timeout = 0) {
  pollfd p{fd, POLLPRI, 0};
  require(poll(&p, 1, timeout) == (urgent ? 1 : 0), urgent ? "poll urgent" : "poll no urgent");
  require(p.revents == (urgent ? POLLPRI : 0), "priority mask");
  fd_set exceptions; FD_ZERO(&exceptions); FD_SET(fd, &exceptions);
  timeval zero{};
  require(select(fd + 1, nullptr, nullptr, &exceptions, &zero) == (urgent ? 1 : 0), "select exception count");
  require((FD_ISSET(fd, &exceptions) != 0) == urgent, "select exception membership");
}
static long long millis() {
  timespec t{}; require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock");
  return static_cast<long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
static void scenario(int inline_oob) {
  Pair a = pair(), other = pair();
  require(setsockopt(a.receiver, SOL_SOCKET, SO_OOBINLINE, &inline_oob, sizeof(inline_oob)) == 0, "inline option");
  ready(a.receiver, false);
  long long start = millis(); ready(a.receiver, false, 60);
  require(millis() - start >= 40 && millis() - start < 1500, "bounded no-data wait");
  int ep = epoll_create1(0); require(ep >= 0, "epoll create");
  epoll_event interest{}; interest.events = EPOLLPRI; interest.data.u64 = 0x12345678;
  require(epoll_ctl(ep, EPOLL_CTL_ADD, a.receiver, &interest) == 0, "epoll add");
  require(send(a.sender, "abc", 3, 0) == 3, "normal send");
  pollfd normal{a.receiver, POLLIN, 0}; require(poll(&normal, 1, 2000) == 1 && normal.revents == POLLIN, "normal data arrived");
  ready(a.receiver, false, 30);
  require(send(a.sender, "!", 1, MSG_OOB) == 1, "urgent send");
  ready(a.receiver, true, 2000); ready(a.receiver, true);
  int duplicate = dup(a.receiver); require(duplicate >= 0, "dup");
  pollfd many[] = {{other.receiver, POLLPRI, 0}, {a.receiver, POLLIN | POLLPRI, 0}, {duplicate, POLLPRI, 0}, {-1, POLLPRI, 99}};
  require(poll(many, 4, 0) == 2 && many[0].revents == 0 && many[1].revents == (POLLIN | POLLPRI) && many[2].revents == POLLPRI && many[3].revents == 0, "multi descriptor aliases and simultaneous normal data");
  epoll_event out{}; require(epoll_wait(ep, &out, 1, 2000) == 1 && out.events == EPOLLPRI && out.data.u64 == interest.data.u64, "epoll urgent");
  require(epoll_wait(ep, &out, 1, 0) == 1 && out.events == EPOLLPRI, "epoll level repoll");
  char bytes[4]{};
  if (!inline_oob) {
    require(recv(duplicate, bytes, 1, MSG_OOB) == 1 && bytes[0] == '!', "consume only urgent byte through alias");
    ready(a.receiver, false); ready(duplicate, false);
    require(epoll_wait(ep, &out, 1, 0) == 0, "epoll urgent cleared before normal read");
  }
  int received = 0;
  while (received < 3) {
    int n = static_cast<int>(recv(a.receiver, bytes + received, 3 - received, 0));
    require(n > 0, "normal prefix progress"); received += n;
  }
  require(memcmp(bytes, "abc", 3) == 0, "normal prefix preserved");
  if (inline_oob) {
    ready(a.receiver, true);
    require(recv(a.receiver, bytes, 1, 0) == 1 && bytes[0] == '!', "inline urgent read");
    ready(a.receiver, false);
    require(epoll_wait(ep, &out, 1, 0) == 0, "inline urgent cleared");
  }
  require(close(duplicate) == 0 && close(ep) == 0, "alias epoll cleanup");
  require(close(a.sender) == 0, "peer close");
  normal = {a.receiver, POLLIN, 0}; require(poll(&normal, 1, 2000) == 1, "peer EOF arrived");
  pollfd eof{a.receiver, POLLPRI, 0}; require(poll(&eof, 1, 0) >= 0 && !(eof.revents & POLLPRI), "EOF is not urgent");
  require(close(a.receiver) == 0 && close(other.sender) == 0 && close(other.receiver) == 0, "cleanup");
  printf("PASS TCP priority: inline=%d queue/repoll/consume/select/epoll/dup/multi/normal/timeout/EOF\n", inline_oob);
}
static void* delayed(void* raw) {
  int fd = *static_cast<int*>(raw);
  usleep(40000);
  require(send(fd, "?", 1, MSG_OOB) == 1, "concurrent urgent send");
  return nullptr;
}
static void finite_priority_control(int fd, const char* phase) {
  pollfd p{fd, POLLPRI, 0};
  const long long start = millis();
  const int rc = poll(&p, 1, 60);
  const long long elapsed = millis() - start;
  printf("TCP_PRIORITY_FINITE phase=%s rc=%d revents=%d elapsed_ms=%lld\n", phase, rc, p.revents, elapsed);
  fflush(stdout);
  require(rc == 0 && p.revents == 0 && elapsed >= 40 && elapsed < 1500, "finite priority-only endpoint wait");
}
static void endpoint_controls() {
  int unconnected = socket(AF_INET, SOCK_STREAM, 0); require(unconnected >= 0, "control fresh stream");
  pollfd initial{unconnected, POLLPRI, 0};
  const int initial_rc = poll(&initial, 1, 60);
  printf("TCP_PRIORITY_INITIAL rc=%d revents=%d\n", initial_rc, initial.revents); fflush(stdout);
  require(initial_rc == 1 && (initial.revents & POLLHUP) && !(initial.revents & POLLPRI), "fresh stream hangup is not priority");
  int initial_alias = dup(unconnected); require(initial_alias >= 0, "control fresh alias");
  pollfd alias_hup{initial_alias, POLLPRI, 0};
  require(poll(&alias_hup, 1, 0) == 1 && (alias_hup.revents & POLLHUP) && !(alias_hup.revents & POLLPRI), "fresh alias hangup is not priority");
  require(close(initial_alias) == 0, "control fresh alias cleanup");
  sockaddr_in bound{}; bound.sin_family = AF_INET; bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(unconnected, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) == 0, "control bound stream");
  initial.revents = 0;
  require(poll(&initial, 1, 0) == 1 && (initial.revents & POLLHUP) && !(initial.revents & POLLPRI), "bound unconnected stream hangup is not priority");
  require(close(unconnected) == 0, "control fresh stream cleanup");
  int listener = socket(AF_INET, SOCK_STREAM, 0); require(listener >= 0, "control listener");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "control listen");
  socklen_t length = sizeof(address);
  require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "control listener name");
  finite_priority_control(listener, "listener");
  int datagram = socket(AF_INET, SOCK_DGRAM, 0); require(datagram >= 0, "control datagram");
  finite_priority_control(datagram, "unconnected-datagram");
  int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); require(client >= 0, "control nonblocking client");
  int rc = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  require(rc == 0 || (rc == -1 && errno == EINPROGRESS), "control successful connect pending");
  // Completion may wake the backend internally, but no requested event exists.
  finite_priority_control(client, "successful-connect");
  int receiver = accept(listener, nullptr, nullptr); require(receiver >= 0, "control accept");
  sockaddr_storage peer{}; length = sizeof(peer);
  require(getpeername(client, reinterpret_cast<sockaddr*>(&peer), &length) == 0, "control connected peer");
  finite_priority_control(client, "connected-client");
  int flags = fcntl(client, F_GETFL);
  require(flags >= 0 && fcntl(client, F_SETFL, flags & ~O_NONBLOCK) == 0, "control blocking client");
  timeval receive_timeout{0, 300000};
  require(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) == 0, "control read deadline");
  // This connected endpoint is writable but has no incoming payload. A raw
  // unrequested write wake must never make MSG_DONTWAIT enter blocking recv.
  char byte{}; const long long read_start = millis();
  const int received = static_cast<int>(recv(client, &byte, 1, MSG_DONTWAIT));
  const int read_error = errno;
  const long long read_elapsed = millis() - read_start;
  require(received == -1 && (read_error == EAGAIN || read_error == EWOULDBLOCK) && read_elapsed < 200, "writable socket is not readable for MSG_DONTWAIT");
  int connected_alias = dup(client); require(connected_alias >= 0, "control connected alias");
  require(close(receiver) == 0, "control connected peer close");
  pollfd eof{client, POLLIN, 0};
  require(poll(&eof, 1, 2000) == 1 && recv(client, &byte, 1, 0) == 0, "control successful connect then EOF");
  pollfd eof_alias{connected_alias, POLLPRI, 0};
  require(poll(&eof_alias, 1, 0) >= 0 && !(eof_alias.revents & POLLPRI), "control completed connect alias EOF not priority");
  require(close(connected_alias) == 0 && close(client) == 0 && close(datagram) == 0 && close(listener) == 0, "control endpoint cleanup");
  puts("PASS TCP priority: listener datagram and successful connect preserve finite priority-only waits");
}
struct ErrorGate { pthread_mutex_t mutex; pthread_cond_t condition; int waiting = 0; bool go = false; };
struct ErrorReader { int fd; ErrorGate* gate; int rc = -99, value = -99; };
static void* consume_error(void* raw) {
  auto* reader = static_cast<ErrorReader*>(raw); auto& gate = *reader->gate;
  require(pthread_mutex_lock(&gate.mutex) == 0, "error reader gate lock");
  ++gate.waiting; require(pthread_cond_broadcast(&gate.condition) == 0, "error reader arrived");
  while (!gate.go) require(pthread_cond_wait(&gate.condition, &gate.mutex) == 0, "error reader start wait");
  require(pthread_mutex_unlock(&gate.mutex) == 0, "error reader gate unlock");
  socklen_t length = sizeof(reader->value);
  reader->rc = getsockopt(reader->fd, SOL_SOCKET, SO_ERROR, &reader->value, &length);
  return nullptr;
}
static void concurrent_error_consumption(int first, int second) {
  ErrorGate gate;
  require(pthread_mutex_init(&gate.mutex, nullptr) == 0 && pthread_cond_init(&gate.condition, nullptr) == 0, "error gate create");
  ErrorReader readers[] = {{first, &gate}, {second, &gate}}; pthread_t threads[2];
  for (int i = 0; i < 2; ++i) require(pthread_create(&threads[i], nullptr, consume_error, &readers[i]) == 0, "error reader thread");
  require(pthread_mutex_lock(&gate.mutex) == 0, "error gate controller lock");
  timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "error gate clock"); deadline.tv_sec += 2;
  while (gate.waiting != 2) require(pthread_cond_timedwait(&gate.condition, &gate.mutex, &deadline) == 0, "error readers bounded rendezvous");
  gate.go = true; require(pthread_cond_broadcast(&gate.condition) == 0 && pthread_mutex_unlock(&gate.mutex) == 0, "release concurrent error readers");
  for (int i = 0; i < 2; ++i) require(pthread_join(threads[i], nullptr) == 0, "join error reader");
  printf("TCP_PRIORITY_CONCURRENT_ERROR first_rc=%d first=%d second_rc=%d second=%d\n", readers[0].rc, readers[0].value, readers[1].rc, readers[1].value); fflush(stdout);
  require(readers[0].rc == 0 && readers[1].rc == 0 &&
          ((readers[0].value == ECONNREFUSED && readers[1].value == 0) || (readers[1].value == ECONNREFUSED && readers[0].value == 0)), "exactly one alias consumes connection error");
  require(pthread_cond_destroy(&gate.condition) == 0 && pthread_mutex_destroy(&gate.mutex) == 0, "error gate cleanup");
}
static void connection_controls() {
  int reserved = socket(AF_INET, SOCK_STREAM, 0); require(reserved >= 0, "reserve refused port");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(reserved, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve bind");
  socklen_t length = sizeof(address);
  require(getsockname(reserved, reinterpret_cast<sockaddr*>(&address), &length) == 0, "reserve name");
  int client = socket(AF_INET, SOCK_STREAM, 0); require(client >= 0, "refused client");
  int flags = fcntl(client, F_GETFL); require(flags >= 0 && fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0, "refused nonblock");
  int connecting_alias = dup(client); require(connecting_alias >= 0, "connect through alias");
  int connected = connect(connecting_alias, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  const int connect_error = errno;
  require(connected == -1 && (errno == EINPROGRESS || errno == ECONNREFUSED), "refused connection pending");
  require(close(connecting_alias) == 0, "connecting alias cleanup");
  pollfd refused{client, POLLPRI, 0};
  const long long start = millis();
  const int polled = poll(&refused, 1, 5000);
  const int poll_error = errno;
  const long long elapsed = millis() - start;
  sockaddr_storage peer{}; length = sizeof(peer);
  const int peer_rc = getpeername(client, reinterpret_cast<sockaddr*>(&peer), &length);
  const int peer_error = peer_rc < 0 ? errno : 0;
  printf("TCP_PRIORITY_REFUSED connect_rc=%d connect_errno=%d poll_rc=%d revents=%d poll_errno=%d elapsed_ms=%lld peer_rc=%d peer_errno=%d SO_ERROR=not-read\n",
         connected, connect_error, polled, refused.revents, poll_error, elapsed, peer_rc, peer_error);
  fflush(stdout);
  require(polled == 1 && (refused.revents & (POLLERR | POLLHUP)) && !(refused.revents & POLLPRI), "refused error never priority");
  require(elapsed < 6500, "refused finite deadline preserved");
  int alias = dup(client); require(alias >= 0, "refused alias");
  pollfd alias_error{alias, POLLPRI, 0};
  require(poll(&alias_error, 1, 0) == 1 && (alias_error.revents & (POLLERR | POLLHUP)) && !(alias_error.revents & POLLPRI), "refused alias error never priority");
  int error = 0; length = sizeof(error);
  const int error_rc = getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length);
  printf("TCP_PRIORITY_SO_ERROR rc=%d value=%d errno=%d length=%u expected=%d\n", error_rc, error, errno, static_cast<unsigned>(length), ECONNREFUSED); fflush(stdout);
  require(error_rc == 0 && error == ECONNREFUSED, "readiness preserved SO_ERROR");
  int alias_value = -1; length = sizeof(alias_value);
  require(getsockopt(alias, SOL_SOCKET, SO_ERROR, &alias_value, &length) == 0 && alias_value == 0, "caller error consumption shared with alias");
  require(close(alias) == 0, "refused alias cleanup");
  error = -1; length = sizeof(error);
  require(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "only caller consumed SO_ERROR");
  pollfd consumed{client, POLLPRI, 0};
  int consumed_rc = poll(&consumed, 1, 0);
  printf("TCP_PRIORITY_ERROR_CONSUMED rc=%d revents=%d\n", consumed_rc, consumed.revents); fflush(stdout);
  require(consumed_rc == 1 && consumed.revents == POLLHUP, "consumed connection error leaves only hangup");
  require(send(client, "x", 1, MSG_NOSIGNAL | MSG_DONTWAIT) == -1 && (errno == EPIPE || errno == ENOTCONN), "send after terminal refusal stays unconnected");
  error = -1; length = sizeof(error);
  require(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "failed send does not resurrect consumed refusal");
  // TCP reconnect after failure is unspecified. Exercise the portable retry:
  // a fresh socket replaces the same fd, with a new shared error generation.
  int fresh = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); require(fresh >= 0, "fresh retry socket");
  require(dup2(fresh, client) == client && close(fresh) == 0, "replace failed descriptor for fresh retry");
  connected = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  require(connected == -1 && (errno == EINPROGRESS || errno == ECONNREFUSED), "fresh retry connection pending");
  refused = {client, POLLPRI, 0};
  require(poll(&refused, 1, 5000) == 1 && (refused.revents & (POLLERR | POLLHUP)) && !(refused.revents & POLLPRI), "fresh retry error never priority");
  int retry_alias = dup(client); require(retry_alias >= 0, "fresh retry error alias");
  concurrent_error_consumption(client, retry_alias);
  require(close(retry_alias) == 0, "fresh retry alias cleanup");
  error = -1; length = sizeof(error);
  require(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "fresh retry error consumed once");
  require(close(client) == 0 && close(reserved) == 0, "refused cleanup");
  pollfd closed{client, POLLPRI, 0};
  require(poll(&closed, 1, 0) == 1 && closed.revents == POLLNVAL, "closed fd is invalid not urgent");
  puts("PASS TCP priority: failed connection and closed descriptor distinguish errors and preserve SO_ERROR");
}
static void peer_eof(int fd) {
  pollfd p{fd, POLLIN, 0}; char byte{};
  require(poll(&p, 1, 2000) == 1 && recv(fd, &byte, 1, 0) == 0, "last alias close produces peer EOF");
}
static int status_flags(int fd) {
  const int flags = fcntl(fd, F_GETFL); require(flags >= 0, "alias status read"); return flags;
}
static void nonblocking_aliases(int first, int second) {
  int flags = status_flags(first);
  require(fcntl(first, F_SETFL, flags | O_NONBLOCK) == 0 && (status_flags(second) & O_NONBLOCK), "F_SETFL status shared toward replacement");
  char byte{};
  require(recv(second, &byte, 1, MSG_DONTWAIT) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "nonblocking empty alias recv");
  require(fcntl(second, F_SETFL, flags & ~O_NONBLOCK) == 0 && !(status_flags(first) & O_NONBLOCK), "F_SETFL status shared toward source");
  timeval timeout{0, 300000};
  require(setsockopt(first, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "alias receive safety deadline");
  const long long start = millis();
  require(recv(first, &byte, 1, MSG_DONTWAIT) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) && millis() - start < 200, "shared blocking status preserves MSG_DONTWAIT");
  int on = 1;
  require(ioctl(second, FIONBIO, &on) == 0 && (status_flags(first) & O_NONBLOCK), "FIONBIO status shared toward source");
  on = 0;
  require(ioctl(first, FIONBIO, &on) == 0 && !(status_flags(second) & O_NONBLOCK), "FIONBIO status shared toward replacement");
  on = 1; require(setsockopt(first, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0, "alias reuse set");
  on = 0; socklen_t length = sizeof(on);
  require(getsockopt(second, SOL_SOCKET, SO_REUSEADDR, &on, &length) == 0 && on == 1, "reuse option shared toward replacement");
  on = 0; require(setsockopt(second, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0, "alias reuse clear");
  on = -1; length = sizeof(on);
  require(getsockopt(first, SOL_SOCKET, SO_REUSEADDR, &on, &length) == 0 && on == 0, "reuse option shared toward source");
}
struct AliasWait { int fd; int rc; short revents; long long elapsed; };
static void* alias_wait(void* raw) {
  auto* wait = static_cast<AliasWait*>(raw);
  pollfd p{wait->fd, POLLPRI, 0}; const long long start = millis();
  wait->rc = poll(&p, 1, 2000); wait->revents = p.revents; wait->elapsed = millis() - start;
  return nullptr;
}
static void alias_ownership() {
  Pair a = pair(), b = pair();
  int old_target = dup(b.receiver); require(old_target >= 0, "alias target copy");
  require(dup2(-1, b.receiver) == -1 && errno == EBADF, "invalid-source dup2 leaves target intact");
  require(send(b.sender, "v", 1, 0) == 1, "failed replacement target peer send");
  char retained{}; require(recv(b.receiver, &retained, 1, 0) == 1 && retained == 'v', "failed replacement retained target");
  require(dup2(a.receiver, b.receiver) == b.receiver, "alias target replacement");
  nonblocking_aliases(a.receiver, b.receiver);
  require(close(a.receiver) == 0, "replacement source close");
  require(send(a.sender, "a", 1, 0) == 1 && send(b.sender, "b", 1, 0) == 1, "alias replacement peer send");
  char byte{};
  require(recv(b.receiver, &byte, 1, 0) == 1 && byte == 'a', "replacement retains source after source close");
  require(recv(old_target, &byte, 1, 0) == 1 && byte == 'b', "replacement retains old target alias");
  require(close(old_target) == 0, "last target alias close"); peer_eof(b.sender);
  require(close(b.receiver) == 0, "last source alias close"); peer_eof(a.sender);
  require(close(a.sender) == 0 && close(b.sender) == 0, "replacement peer cleanup");

  a = pair(); int sibling = dup(a.receiver); require(sibling >= 0, "polling sibling alias");
  AliasWait wait{sibling, -99, 0, 0}; pthread_t thread;
  require(pthread_create(&thread, nullptr, alias_wait, &wait) == 0, "alias waiter thread");
  usleep(40000); require(close(a.receiver) == 0, "close sibling while alias polls");
  usleep(40000); require(send(a.sender, "!", 1, MSG_OOB) == 1, "urgent after sibling close");
  require(pthread_join(thread, nullptr) == 0 && wait.rc == 1 && wait.revents == POLLPRI && wait.elapsed < 2000, "sibling close preserves bounded alias urgent wait");
  require(recv(sibling, &byte, 1, MSG_OOB) == 1 && byte == '!', "alias urgent payload after sibling close");
  require(close(sibling) == 0, "last polling alias close"); peer_eof(a.sender);
  require(close(a.sender) == 0, "polling peer cleanup");

  int sender = socket(AF_INET, SOCK_DGRAM, 0), receiver = socket(AF_INET, SOCK_DGRAM, 0);
  require(sender >= 0 && receiver >= 0, "datagram alias endpoints");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "datagram alias bind");
  socklen_t length = sizeof(address);
  require(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &length) == 0, "datagram alias address");
  int alias = dup(receiver); require(alias >= 0, "datagram alias");
  nonblocking_aliases(receiver, alias);
  require(close(receiver) == 0, "datagram alias retains endpoint");
  require(sendto(sender, "u", 1, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 1, "datagram alias send");
  pollfd p{alias, POLLIN, 0};
  require(poll(&p, 1, 2000) == 1 && recv(alias, &byte, 1, 0) == 1 && byte == 'u', "datagram alias receives unchanged payload");
  int challenger = socket(AF_INET, SOCK_DGRAM, 0); require(challenger >= 0, "datagram binding challenger");
  require(bind(challenger, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == EADDRINUSE, "UDP port retained until last alias close");
  require(close(alias) == 0, "datagram last alias close");
  require(bind(challenger, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "UDP port released by last alias close");
  require(close(alias) == -1 && errno == EBADF, "closed UDP alias remains invalid");
  require(close(challenger) == 0 && close(sender) == 0, "datagram alias cleanup");
  puts("PASS TCP priority: shared aliases preserve dup2 targets last-close EOF concurrent urgent poll and UDP payload");
}
int main(int argc, char** argv) {
  const bool noninline_only = argc == 2 && strcmp(argv[1], "--noninline") == 0;
  require(argc == 1 || noninline_only, "explicit supported scope");
  scenario(0);
  if (!noninline_only) scenario(1);
  endpoint_controls();
  connection_controls();
  alias_ownership();
  Pair a = pair(); pthread_t thread;
  require(pthread_create(&thread, nullptr, delayed, &a.sender) == 0, "sender thread");
  ready(a.receiver, true, 2000);
  require(pthread_join(thread, nullptr) == 0, "sender joined");
  char byte{}; require(recv(a.receiver, &byte, 1, MSG_OOB) == 1 && byte == '?', "concurrent byte preserved");
  ready(a.receiver, false, 30);
  require(close(a.sender) == 0 && close(a.receiver) == 0, "concurrent cleanup");
  puts("PASS TCP priority: delayed arrival bounded wake and cleanup");
  if (noninline_only) puts("PASS TCP priority: noninline contract checks passed; inline remains OPEN");
  else puts("PASS TCP priority: all contract checks passed");
}
