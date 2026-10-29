// The identical contract is compiled against Linux, Gesher host POSIX, and both NDK ABIs.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef GESHER
#include <sys/eventfd.h>
#include "../../compat/posix/src/socket_internal.h"
namespace gesher::test { int NativeUdpErrorControl(); }
#endif

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
  const long long empty_start = millis();
  require(poll(nullptr, 0, 60) == 0 && millis() - empty_start >= 40 && millis() - empty_start < 1500, "count-zero poll retains its finite deadline");
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
struct ConnectWindow {
  int client; sockaddr_in address;
  pthread_mutex_t mutex; pthread_cond_t condition;
  bool native_ready = false, release = false, getter_entered = false, getter_done = false;
  int metadata_lock_busy = -1;
  int connect_rc = -99, connect_error = -99, getter_rc = -99, getter_value = -99;
};
static void pause_after_native(uintptr_t native, int result, int error, void* raw) {
  auto& window = *static_cast<ConnectWindow*>(raw);
  require(result == -1 && (error == EINPROGRESS || error == ECONNREFUSED), "window real connect result");
#ifdef GESHER
  gesher::ws::PollEntry p{native, gesher::ws::kPollOut, 0, nullptr};
  require(gesher::ws::Poll(&p, 1, 5000) == 1 && (p.revents & gesher::ws::kPollErr), "window actual native refusal");
#else
  (void)native;
  pollfd p{window.client, POLLOUT, 0};
  require(poll(&p, 1, 5000) == 1 && (p.revents & POLLERR), "window actual POSIX refusal");
#endif
  require(pthread_mutex_lock(&window.mutex) == 0, "window observer lock");
  window.native_ready = true; require(pthread_cond_broadcast(&window.condition) == 0, "window native readiness publication");
  while (!window.release) require(pthread_cond_wait(&window.condition, &window.mutex) == 0, "window post-native barrier");
  require(pthread_mutex_unlock(&window.mutex) == 0, "window observer unlock");
}
static void* window_connect(void* raw) {
  auto& window = *static_cast<ConnectWindow*>(raw);
#ifdef GESHER
  window.connect_rc = gesher::ConnectWithObserver(window.client, &window.address, sizeof(window.address), pause_after_native, &window);
  window.connect_error = errno;
#else
  window.connect_rc = connect(window.client, reinterpret_cast<sockaddr*>(&window.address), sizeof(window.address));
  window.connect_error = errno;
  pause_after_native(static_cast<uintptr_t>(window.client), window.connect_rc, window.connect_error, &window);
#endif
  return nullptr;
}
static void* window_getter(void* raw) {
  auto& window = *static_cast<ConnectWindow*>(raw); socklen_t length = sizeof(window.getter_value);
#ifdef GESHER
  gesher::SocketFd state;
  require(gesher::LookupSocket(window.client, &state), "window exact descriptor snapshot");
  const bool entered = state.description->error_mutex.try_lock();
  if (entered) state.description->error_mutex.unlock();
  window.metadata_lock_busy = entered ? 0 : 1;
#endif
  require(pthread_mutex_lock(&window.mutex) == 0, "window getter entry lock");
  window.getter_entered = true; require(pthread_cond_broadcast(&window.condition) == 0, "window getter entered");
  require(pthread_mutex_unlock(&window.mutex) == 0, "window getter entry unlock");
  window.getter_rc = getsockopt(window.client, SOL_SOCKET, SO_ERROR, &window.getter_value, &length);
  require(pthread_mutex_lock(&window.mutex) == 0, "window getter lock");
  window.getter_done = true;
  require(pthread_cond_broadcast(&window.condition) == 0, "window getter completed publication");
  require(pthread_mutex_unlock(&window.mutex) == 0, "window getter unlock");
  return nullptr;
}
static void connect_finalization_window() {
  int reserved = socket(AF_INET, SOCK_STREAM, 0); require(reserved >= 0, "window reserved port");
  ConnectWindow window{}; window.address.sin_family = AF_INET; window.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(reserved, reinterpret_cast<sockaddr*>(&window.address), sizeof(window.address)) == 0, "window reserve bind");
  socklen_t length = sizeof(window.address);
  require(getsockname(reserved, reinterpret_cast<sockaddr*>(&window.address), &length) == 0, "window reserved address");
  window.client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); require(window.client >= 0, "window nonblocking client");
  require(pthread_mutex_init(&window.mutex, nullptr) == 0 && pthread_cond_init(&window.condition, nullptr) == 0, "window gate initialize");
  pthread_t connecting, getter;
  require(pthread_create(&connecting, nullptr, window_connect, &window) == 0, "window connect thread");
  require(pthread_mutex_lock(&window.mutex) == 0, "window controller lock");
  timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "window clock"); deadline.tv_sec += 7;
  while (!window.native_ready) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "window bounded native readiness");
  require(pthread_mutex_unlock(&window.mutex) == 0, "window controller unlock");
  require(pthread_create(&getter, nullptr, window_getter, &window) == 0, "window getter thread");
  require(pthread_mutex_lock(&window.mutex) == 0, "window getter observation lock");
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "window getter clock"); deadline.tv_sec += 2;
  while (!window.getter_done) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "actual caller getter completes while requester remains paused");
  const bool before_release = window.getter_done;
  require(before_release && window.getter_entered, "getter progress independent of paused requester");
  window.release = true; require(pthread_cond_broadcast(&window.condition) == 0 && pthread_mutex_unlock(&window.mutex) == 0, "release real post-native window");
  require(pthread_join(connecting, nullptr) == 0 && pthread_join(getter, nullptr) == 0, "window threads retired");
  require(window.connect_rc == -1 && (window.connect_error == EINPROGRESS || window.connect_error == ECONNREFUSED), "window public connect result retained");
  require(window.getter_rc == 0 && window.getter_value == ECONNREFUSED, "window caller gets actual refusal");
  int second = -1; length = sizeof(second);
  require(getsockopt(window.client, SOL_SOCKET, SO_ERROR, &second, &length) == 0 && second == 0, "window refusal consumed exactly once across connect finalization");
#ifdef GESHER
  const char* backend = "host-private";
#else
  const char* backend = "public-posix";
#endif
  printf("TCP_PRIORITY_CONNECT_WINDOW backend=%s native_ready=1 lock_busy=%d getter_before_release=%d first=%d second=%d\n", backend, window.metadata_lock_busy, before_release ? 1 : 0, window.getter_value, second);
  require(close(window.client) == 0 && close(reserved) == 0, "window endpoints retired");
  require(pthread_cond_destroy(&window.condition) == 0 && pthread_mutex_destroy(&window.mutex) == 0, "window gate retired");
  puts("PASS TCP priority: actual connect finalization window consumes refusal once");
}
#ifdef GESHER
struct SubmitWindow : ConnectWindow {
  int mode = 0;
  long long elapsed = -1;
  std::shared_ptr<gesher::ws::ConnectOperation> operation;
};
static void submitted_barrier(uintptr_t native, int result, int error, void* raw) {
  auto& window = *static_cast<SubmitWindow*>(raw);
  require(result == -1 && error == EINPROGRESS, "private barrier observes real queued submission");
  gesher::ws::PollEntry p{native, gesher::ws::kPollOut, 0, nullptr};
  require(gesher::ws::Poll(&p, 1, 0) == 0, "private barrier native operation actually remains pending");
  gesher::SocketFd state; require(gesher::LookupSocket(window.client, &state), "private barrier exact socket");
  { std::lock_guard<std::mutex> lock(state.description->error_mutex); window.operation = state.description->connect_operation; }
  require(window.operation != nullptr, "private barrier retains actual operation lifetime");
  require(pthread_mutex_lock(&window.mutex) == 0, "submission barrier lock");
  window.native_ready = true; require(pthread_cond_broadcast(&window.condition) == 0, "submission barrier publication");
  // Cancellation/timeout cases return directly into the real production wait,
  // rather than cancelling a fixture condvar in place of that wait.
  if (window.mode < 2) while (!window.release) require(pthread_cond_wait(&window.condition, &window.mutex) == 0, "submission paused before logical wait/return");
  require(pthread_mutex_unlock(&window.mutex) == 0, "submission barrier unlock");
}
static void* submission_connect(void* raw) {
  auto& window = *static_cast<SubmitWindow*>(raw); const long long start = millis();
  window.connect_rc = gesher::ConnectWithObserver(window.client, &window.address, sizeof(window.address), submitted_barrier, &window);
  window.connect_error = errno; window.elapsed = millis() - start; return nullptr;
}
static void owned_wait_controls() {
  gesher::ws::OwnerCensus baseline{};
  for (int mode = 0; mode != 4; ++mode) {
    SubmitWindow window{}; window.mode = mode;
    int reserved = socket(AF_INET, SOCK_STREAM, 0); require(reserved >= 0, "owned wait reserve");
    window.address.sin_family = AF_INET; window.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(reserved, reinterpret_cast<sockaddr*>(&window.address), sizeof(window.address)) == 0, "owned wait reserve bind");
    socklen_t length = sizeof(window.address); require(getsockname(reserved, reinterpret_cast<sockaddr*>(&window.address), &length) == 0, "owned wait reserve address");
    window.client = socket(AF_INET, SOCK_STREAM | (mode == 1 ? SOCK_NONBLOCK : 0), 0); require(window.client >= 0, "owned wait client");
    int alias = dup(window.client); require(alias >= 0, "owned wait alias");
    if (mode == 3) { timeval timeout{0, 60000}; require(setsockopt(alias, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "owned wait send deadline"); }
    require(pthread_mutex_init(&window.mutex, nullptr) == 0 && pthread_cond_init(&window.condition, nullptr) == 0, "owned wait gate");
    pthread_t caller; require(pthread_create(&caller, nullptr, submission_connect, &window) == 0, "owned wait actual caller");
    require(pthread_mutex_lock(&window.mutex) == 0, "owned wait controller lock");
    timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "owned wait clock"); deadline.tv_sec += 2;
    while (!window.native_ready) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "owned wait actual native submission deadline");
    require(pthread_mutex_unlock(&window.mutex) == 0, "owned wait controller unlock");
    if (mode < 2) {
      int flags = fcntl(alias, F_GETFL); require(flags >= 0, "owned wait current flags");
      const bool now_nonblocking = mode == 0;
      require(fcntl(alias, F_SETFL, now_nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0, "actual mode flip while owned native call is pending");
      require(((fcntl(window.client, F_GETFL) & O_NONBLOCK) != 0) == now_nonblocking, "mode flip shared with initiating alias");
      require(pthread_mutex_lock(&window.mutex) == 0, "owned wait release lock"); window.release = true;
      require(pthread_cond_broadcast(&window.condition) == 0 && pthread_mutex_unlock(&window.mutex) == 0, "owned wait release");
    } else if (mode == 2) require(pthread_cancel(caller) == 0, "cancel real logical blocking connect waiter");
    void* returned = nullptr; require(pthread_join(caller, &returned) == 0, "owned wait caller really retired");
    if (mode == 2) require(returned == PTHREAD_CANCELED, "real connect wait is a deferred cancellation point");
    else require(window.connect_rc == -1 && window.connect_error == (mode == 0 ? ECONNREFUSED : EINPROGRESS), "logical call mode remains fixed across native mode change");
    if (mode == 3) require(window.elapsed >= 40 && window.elapsed < 1500, "logical send timeout leaves a live pending operation");
    if (mode == 1 || mode == 2 || mode == 3) {
      pollfd p{alias, POLLPRI, 0}; require(poll(&p, 1, 5000) == 1 && (p.revents & POLLERR), "flipped-to-blocking alias retains nonblocking completion token");
      int error = 0; length = sizeof(error); require(getsockopt(alias, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == ECONNREFUSED, "flipped alias receives exact completion");
    }
    require(close(window.client) == 0 && close(alias) == 0, "owned wait last close");
    require(gesher::ws::AwaitConnectRetirement(window.operation, 2000) == 0, "owned wait native owner and reaper really exit");
    printf("TCP_PRIORITY_OWNED_WAIT mode=%d connect_rc=%d connect_errno=%d canceled=%d elapsed_ms=%lld native_retired=1\n", mode, window.connect_rc, window.connect_error, returned == PTHREAD_CANCELED ? 1 : 0, window.elapsed); fflush(stdout);
    require(close(reserved) == 0 && pthread_cond_destroy(&window.condition) == 0 && pthread_mutex_destroy(&window.mutex) == 0, "owned wait gate cleanup");
    window.operation.reset();  // all caller context handles retire before census
    gesher::ws::OwnerCensus actual{}; require(gesher::ws::ReadConnectOwnerCensus(&actual), "actual production owner handle/thread census");
    if (mode == 0) baseline = actual;
    printf("TCP_PRIORITY_OWNER_CENSUS mode=%d handles=%u/%u threads=%u/%u\n", mode, actual.handles, baseline.handles, actual.threads, baseline.threads); fflush(stdout);
    require(actual.handles == baseline.handles && actual.threads == baseline.threads, "repeated real ownership returns to warm handle and thread baseline");
  }
  puts("PASS TCP priority: real owned mode flips cancellation send deadline and native owner retirement");
}
struct PollWindow {
  pthread_mutex_t mutex; pthread_cond_t condition;
  bool captured = false, release = false;
  bool native_window = false;
  pollfd records[3]{}; size_t count = 0;
  int native_result = -99, native_error = -99;
  int result = -99; long long elapsed = -1;
};
static void pause_poll_snapshot(void* raw) {
  auto& window = *static_cast<PollWindow*>(raw);
  require(pthread_mutex_lock(&window.mutex) == 0, "poll snapshot lock");
  window.captured = true; require(pthread_cond_broadcast(&window.condition) == 0, "actual poll snapshots published");
  timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "poll snapshot clock"); deadline.tv_sec += 2;
  while (!window.release) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "bounded held poll snapshot");
  require(pthread_mutex_unlock(&window.mutex) == 0, "poll snapshot unlock");
}
static void observe_native_poll(bool before, int result, int error, void* raw) {
  auto& window = *static_cast<PollWindow*>(raw);
  if (before) pause_poll_snapshot(raw);
  else { window.native_result = result; window.native_error = error; }
}
static void* snapshot_poll(void* raw) {
  auto& window = *static_cast<PollWindow*>(raw); const long long start = millis();
  window.result = gesher::PollWithObserver(window.records, window.count, 1500,
      window.native_window ? nullptr : pause_poll_snapshot, &window,
      window.native_window ? observe_native_poll : nullptr, &window);
  window.elapsed = millis() - start; return nullptr;
}
static void poll_close_reuse_controls() {
  for (int mode = 0; mode != 2; ++mode) {
    Pair old = pair(); const int victim = old.receiver;
    int sibling = mode ? dup(victim) : -1;
    if (mode) require(sibling >= 0, "poll snapshot retained sibling");
    int udp = socket(AF_INET, SOCK_DGRAM, 0), sender = socket(AF_INET, SOCK_DGRAM, 0);
    require(udp >= 0 && sender >= 0, "poll snapshot UDP control");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "poll snapshot UDP bind");
    socklen_t length = sizeof(address); require(getsockname(udp, reinterpret_cast<sockaddr*>(&address), &length) == 0, "poll snapshot UDP address");
    gesher::SocketFd before; require(gesher::LookupSocket(victim, &before), "poll snapshot original native identity");
    PollWindow window{}; window.records[0] = {victim, POLLPRI, 0}; window.records[1] = {udp, POLLIN, 0};
    if (mode) window.records[2] = {sibling, POLLPRI, 0};
    window.count = mode ? 3 : 2;
    require(pthread_mutex_init(&window.mutex, nullptr) == 0 && pthread_cond_init(&window.condition, nullptr) == 0, "poll snapshot gate");
    pthread_t waiter; require(pthread_create(&waiter, nullptr, snapshot_poll, &window) == 0, "real poll caller");
    require(pthread_mutex_lock(&window.mutex) == 0, "poll snapshot controller lock");
    timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "poll snapshot controller clock"); deadline.tv_sec += 2;
    while (!window.captured) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "real snapshots captured before replacement");
    require(pthread_mutex_unlock(&window.mutex) == 0, "poll snapshot controller unlock");
    Pair replacement{-1, -1};
    if (mode) {
      replacement = pair();
      require(dup2(replacement.receiver, victim) == victim, "actual dup2 replaces captured identity while old alias survives");
      require(send(old.sender, "!", 1, MSG_OOB) == 1 && send(replacement.sender, "x", 1, MSG_OOB) == 1, "independent urgent bytes on old and replacement transports");
    } else {
      require(close(victim) == 0, "actual last close after poll captured native handle");
      int fresh = socket(AF_INET, SOCK_STREAM, 0); require(fresh >= 0, "new native endpoint after last close");
      if (fresh != victim) require(dup2(fresh, victim) == victim && close(fresh) == 0, "force exact captured fd reuse");
    }
    gesher::SocketFd after; require(gesher::LookupSocket(victim, &after) && after.description != before.description, "replacement has distinct actual description");
    require(sendto(sender, "u", 1, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 1, "simultaneously ready independent UDP entry");
    require(pthread_mutex_lock(&window.mutex) == 0, "poll snapshot release lock"); window.release = true;
    require(pthread_cond_broadcast(&window.condition) == 0 && pthread_mutex_unlock(&window.mutex) == 0, "release actual stale snapshot");
    require(pthread_join(waiter, nullptr) == 0, "actual poll caller retired");
    printf("TCP_PRIORITY_POLL_REUSE mode=%d same_fd=1 same_native=%d rc=%d victim=%d udp=%d alias=%d elapsed_ms=%lld\n", mode, after.socket == before.socket ? 1 : 0, window.result, window.records[0].revents, window.records[1].revents, window.records[2].revents, window.elapsed); fflush(stdout);
    require(window.result == (mode ? 3 : 2) && window.records[0].revents == POLLNVAL && window.records[1].revents == POLLIN && window.elapsed < 1500, "stale identity fails closed while all other ready entries and original deadline survive");
    char byte{}; require(recv(udp, &byte, 1, 0) == 1 && byte == 'u', "poll observation preserves independent UDP payload");
    if (mode) {
      require(window.records[2].revents == POLLPRI && recv(sibling, &byte, 1, MSG_OOB) == 1 && byte == '!', "old alias urgent payload remains independent");
      require(recv(victim, &byte, 1, MSG_OOB) == 1 && byte == 'x', "replacement urgent payload was never consumed by stale poll");
      require(close(sibling) == 0 && close(replacement.receiver) == 0 && close(replacement.sender) == 0, "poll replacement aliases cleanup");
    }
    require(close(victim) == 0 && close(old.sender) == 0 && close(udp) == 0 && close(sender) == 0, "poll snapshot endpoints cleanup");
    require(pthread_cond_destroy(&window.condition) == 0 && pthread_mutex_destroy(&window.mutex) == 0, "poll snapshot gate cleanup");
  }
  puts("PASS TCP priority: real poll snapshots reject closed reused and dup2 identities while aliases and other entries remain ready");
}
static void poll_native_error_controls() {
  for (int mode = 0; mode != 3; ++mode) {
    Pair old = pair(); const int victim = old.receiver;
    int other = -1, sender = -1;
    sockaddr_in address{};
    if (mode == 1) { other = eventfd(1, EFD_NONBLOCK); require(other >= 0, "simultaneously ready actual non-socket eventfd"); }
    if (mode == 2) {
      other = socket(AF_INET, SOCK_DGRAM, 0); sender = socket(AF_INET, SOCK_DGRAM, 0);
      require(other >= 0 && sender >= 0, "native error independent UDP endpoints");
      address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      require(bind(other, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "native error UDP bind");
      socklen_t length = sizeof(address); require(getsockname(other, reinterpret_cast<sockaddr*>(&address), &length) == 0, "native error UDP address");
    }
    PollWindow window{}; window.native_window = true; window.records[0] = {victim, POLLPRI, 0};
    window.count = mode ? 2 : 1;
    if (mode) window.records[1] = {other, POLLIN, 0};
    require(pthread_mutex_init(&window.mutex, nullptr) == 0 && pthread_cond_init(&window.condition, nullptr) == 0, "native poll gate");
    pthread_t waiter; require(pthread_create(&waiter, nullptr, snapshot_poll, &window) == 0, "native poll race caller");
    require(pthread_mutex_lock(&window.mutex) == 0, "native poll race controller");
    timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "native poll race clock"); deadline.tv_sec += 2;
    while (!window.captured) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "real finalized native poll inputs captured");
    require(pthread_mutex_unlock(&window.mutex) == 0, "native poll race controller unlock");
    require(close(victim) == 0, "sole captured native socket physically closed before native call");
    if (mode == 1) require(dup2(other, victim) == victim, "exact fd reused by non-socket while native array has no valid sockets");
    if (mode == 2) require(sendto(sender, "n", 1, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 1, "live native entry ready beside stale socket");
    require(pthread_mutex_lock(&window.mutex) == 0, "native poll release lock"); window.release = true;
    require(pthread_cond_broadcast(&window.condition) == 0 && pthread_mutex_unlock(&window.mutex) == 0, "release real native poll entry");
    require(pthread_join(waiter, nullptr) == 0, "native poll caller retired");
    printf("TCP_PRIORITY_NATIVE_POLL_CLOSE mode=%d native_rc=%d native_error=%d rc=%d victim=%d other=%d elapsed_ms=%lld\n",
        mode, window.native_result, window.native_error, window.result, window.records[0].revents, window.records[1].revents, window.elapsed); fflush(stdout);
    require(window.result == (mode ? 2 : 1) && window.records[0].revents == POLLNVAL && window.elapsed < 1500, "native sample error preserves closed identity and original deadline");
    if (mode < 2) require(window.native_result == -1 && window.native_error == 10022, "real all-invalid native array returns documented WSAEINVAL");
    if (mode) require(window.records[1].revents == POLLIN, "native sample error preserves simultaneous non-socket or socket readiness");
    if (mode == 1) {
      uint64_t value = 0; require(read(other, &value, sizeof(value)) == static_cast<int>(sizeof(value)) && value == 1, "native error recovery does not consume eventfd payload");
      require(close(victim) == 0, "reused non-socket target cleanup");
    }
    if (mode == 2) { char byte{}; require(recv(other, &byte, 1, 0) == 1 && byte == 'n', "native error recovery preserves UDP payload"); require(close(sender) == 0, "native error UDP sender cleanup"); }
    if (mode) require(close(other) == 0, "native error simultaneous endpoint cleanup");
    require(close(old.sender) == 0, "native error peer cleanup");
    require(pthread_cond_destroy(&window.condition) == 0 && pthread_mutex_destroy(&window.mutex) == 0, "native poll gate cleanup");
  }
  gesher::ws::PollEntry invalid{gesher::ws::kInvalidSocket, gesher::ws::kPollPri, 0, {}};
  const int result = gesher::ws::Poll(&invalid, 1, 0); const int error = errno;
  printf("TCP_PRIORITY_NATIVE_POLL_ERROR rc=%d errno=%d retired_description=0\n", result, error); fflush(stdout);
  require(result == -1 && error == EINVAL, "genuine unowned native array failure is not rewritten as readiness");
  puts("PASS TCP priority: real all-invalid native poll errors preserve closed identity simultaneous readiness and genuine failures");
}
struct LegacyWindow : ConnectWindow {
  int mode = 0, close_result = -99, replacement = -1;
  std::atomic<bool> close_done{false};
};
static void hold_legacy_native(uintptr_t, int result, int, void* raw) {
  if (result != -2) return;  // distinct immutable before-native dependency
  auto& window = *static_cast<LegacyWindow*>(raw);
  gesher::SocketFd state; require(gesher::LookupSocket(window.client, &state), "legacy exact descriptor before close");
  { std::lock_guard<std::mutex> lock(state.description->error_mutex);
    require(state.description->legacy_connect_active && state.description->refusal_generation == 0, "legacy attempt invalidates classification before native call"); }
  require(pthread_mutex_lock(&window.mutex) == 0, "legacy lease gate lock");
  window.native_ready = true; require(pthread_cond_broadcast(&window.condition) == 0, "legacy call lease really captured");
  timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "legacy lease clock"); deadline.tv_sec += 2;
  while (!window.release) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "bounded actual legacy lease");
  require(pthread_mutex_unlock(&window.mutex) == 0, "legacy lease gate unlock");
}
static void* legacy_connect(void* raw) {
  auto& window = *static_cast<LegacyWindow*>(raw);
  window.connect_rc = gesher::ConnectWithObserver(window.client, &window.address, sizeof(window.address), hold_legacy_native, &window);
  window.connect_error = errno; return nullptr;
}
static void* legacy_close(void* raw) {
  auto& window = *static_cast<LegacyWindow*>(raw);
  window.close_result = window.mode == 3 ? dup2(window.replacement, window.client) : close(window.client);
  window.close_done.store(true, std::memory_order_release); return nullptr;
}
static void legacy_lifetime_controls() {
  for (int mode = 0; mode != 4; ++mode) {
    LegacyWindow window{}; window.mode = mode;
    const bool datagram = mode == 1 || mode == 3;
    const int type = datagram ? SOCK_DGRAM : SOCK_STREAM;
    int target = socket(AF_INET, type, 0); require(target >= 0, "legacy reserve target");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(target, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "legacy reserve bind");
    socklen_t length = sizeof(address); require(getsockname(target, reinterpret_cast<sockaddr*>(&address), &length) == 0, "legacy target address");
    window.client = socket(AF_INET, type | (mode == 2 ? SOCK_NONBLOCK : 0), 0); require(window.client >= 0, "legacy client");
    if (mode == 2) {
      require(connect(window.client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == EINPROGRESS, "legacy actual prior owned pending refusal");
      pollfd p{window.client, POLLPRI, 0}; require(poll(&p, 1, 5000) == 1 && (p.revents & POLLERR), "legacy actual prior owned refusal");
      int error = 0; length = sizeof(error); require(getsockopt(window.client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == ECONNREFUSED, "legacy prior caller consumed refusal");
    }
    window.address = datagram ? address : sockaddr_in{};
    if (!datagram) window.address.sin_family = AF_UNSPEC;
    if (mode == 3) { window.replacement = socket(AF_INET, type, 0); require(window.replacement >= 0, "legacy dup2 source replacement"); }
    gesher::SocketFd before; require(gesher::LookupSocket(window.client, &before), "legacy original open description");
    require(before.description->provider == (datagram ? gesher::ws::Provider::kNativeIpv4Udp : gesher::ws::Provider::kNativeIpv4Tcp), "actual qualified legacy base provider");
    require(pthread_mutex_init(&window.mutex, nullptr) == 0 && pthread_cond_init(&window.condition, nullptr) == 0, "legacy barrier gate");
    pthread_t caller, closer; require(pthread_create(&caller, nullptr, legacy_connect, &window) == 0, "real legacy caller");
    require(pthread_mutex_lock(&window.mutex) == 0, "legacy controller lock");
    timespec deadline{}; require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "legacy controller clock"); deadline.tv_sec += 2;
    while (!window.native_ready) require(pthread_cond_timedwait(&window.condition, &window.mutex, &deadline) == 0, "legacy real pre-native lease");
    require(pthread_mutex_unlock(&window.mutex) == 0, "legacy controller unlock");
    require(pthread_create(&closer, nullptr, legacy_close, &window) == 0, "actual concurrent last close");
    gesher::SocketFd probe; const long long committed_deadline = millis() + 1500; bool committed = false;
    do {
      const bool mapped = gesher::LookupSocket(window.client, &probe);
      committed = mode == 3 ? mapped && probe.description != before.description : !mapped;
      if (!committed) usleep(1000);
    } while (!committed && millis() < committed_deadline);
    require(committed && !window.close_done.load(std::memory_order_acquire), "CRT/table close or replacement commits while physical close retains exact native lease");
    int fresh = socket(AF_INET, type, 0); require(fresh >= 0, "global descriptor operations progress while legacy lease is held");
    if (mode == 3) require(close(fresh) == 0, "independent descriptor close progresses while old native lease is held");
    else if (fresh != window.client) require(dup2(fresh, window.client) == window.client && close(fresh) == 0, "force exact legacy fd reuse");
    gesher::SocketFd after; require(gesher::LookupSocket(window.client, &after) && after.description != before.description && after.socket != before.socket, "leased old native handle cannot become the replacement endpoint");
    int error = -1; length = sizeof(error); require(getsockopt(window.client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "replacement getter progresses without waiting for old native lease");
    require(pthread_mutex_lock(&window.mutex) == 0, "legacy release lock"); window.release = true;
    require(pthread_cond_broadcast(&window.condition) == 0 && pthread_mutex_unlock(&window.mutex) == 0, "legacy lease release");
    require(pthread_join(caller, nullptr) == 0 && pthread_join(closer, nullptr) == 0, "legacy caller and physical closer actually retire");
    require(window.connect_rc == -1 && window.connect_error == EBADF && window.close_result == (mode == 3 ? window.client : 0), "old native call fails closed before touching replacement");
    sockaddr_in peer{}; length = sizeof(peer);
    require(getpeername(window.client, reinterpret_cast<sockaddr*>(&peer), &length) == -1 && errno == ENOTCONN, "replacement was never connected or disconnected by stale caller");
    printf("TCP_PRIORITY_LEGACY_LEASE mode=%d fd_reused=1 old_call_rc=%d old_call_errno=%d close_rc=%d global_progress=1 replacement_error=%d\n", mode, window.connect_rc, window.connect_error, window.close_result, error); fflush(stdout);
    if (mode == 3) require(close(window.replacement) == 0, "legacy original dup2 source cleanup");
    require(close(window.client) == 0 && close(target) == 0 && pthread_cond_destroy(&window.condition) == 0 && pthread_mutex_destroy(&window.mutex) == 0, "legacy barrier owned cleanup");
  }
  puts("PASS TCP priority: real legacy leases reject stale native calls after CRT reuse while global operations and getters progress");
}
static void native_udp_error_control() {
  const int fd = gesher::test::NativeUdpErrorControl();
  pollfd p{fd, POLLIN | POLLPRI, 0};
  require(poll(&p, 1, 0) == 1 && (p.revents & POLLERR) && !(p.revents & POLLPRI), "positive UDP error is never filtered as a consumed TCP refusal");
  char byte{}; require(recv(fd, &byte, 1, MSG_DONTWAIT) == -1 && errno == ECONNRESET, "actual caller receives native UDP error");
  require(close(fd) == 0, "native UDP positive input cleanup");
  puts("PASS TCP priority: positive native UDP error remains visible outside owned TCP refusal scope");
}
#endif
static void failed_to_listener_control() {
  int reserved = socket(AF_INET, SOCK_STREAM, 0); require(reserved >= 0, "listener transition reserve");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(reserved, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "listener transition target bind");
  socklen_t length = sizeof(address); require(getsockname(reserved, reinterpret_cast<sockaddr*>(&address), &length) == 0, "listener transition target address");
  // Actual Linux409301 permits listen after blocking connect consumed the
  // refusal. A failed nonblocking socket remains SS_CONNECTING and rejects it.
  int client = socket(AF_INET, SOCK_STREAM, 0); require(client >= 0, "listener transition client");
  const int connected = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)); const int connect_error = errno;
  printf("TCP_PRIORITY_LISTENER_FAILURE connect_rc=%d connect_errno=%d\n", connected, connect_error); fflush(stdout);
  require(connected == -1 && connect_error == ECONNREFUSED, "listener transition blocking caller consumed actual refusal");
  pollfd p{client, POLLPRI, 0};
  int error = -1; length = sizeof(error); require(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "blocking caller already consumed old connection error");
  const int rebound = bind(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)); const int bind_error = errno;
  printf("TCP_PRIORITY_LISTENER_TRANSITION bind_rc=%d bind_errno=%d\n", rebound, bind_error); fflush(stdout);
  require(rebound == -1, "an already bound failed socket does not change its local address");
  require(listen(client, 1) == 0, "actual failed socket becomes a listener");
  finite_priority_wait(client, "failed-socket-now-listening");
  length = sizeof(address); require(getsockname(client, reinterpret_cast<sockaddr*>(&address), &length) == 0 && address.sin_port != 0, "actual transitioned listener address"); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int incoming = socket(AF_INET, SOCK_STREAM, 0); require(incoming >= 0 && connect(incoming, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "new transport reaches transitioned listener");
  p = {client, POLLIN, 0}; require(poll(&p, 1, 2000) == 1, "transitioned listener accepts new connection");
  int accepted = accept(client, nullptr, nullptr); require(accepted >= 0, "transitioned accepted endpoint");
  require(send(incoming, "!", 1, MSG_OOB) == 1, "transitioned transport urgent payload"); ready(accepted, true, 2000);
  char byte{}; require(recv(accepted, &byte, 1, MSG_OOB) == 1 && byte == '!', "transitioned transport urgent payload preserved");
  require(close(accepted) == 0 && close(incoming) == 0 && close(client) == 0 && close(reserved) == 0, "listener transition cleanup");
  puts("PASS TCP priority: bind and listen transitions invalidate old refusal classification before new transport");
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
  require(send(client, "!", 1, MSG_NOSIGNAL | MSG_DONTWAIT | MSG_OOB) == -1 && (errno == EPIPE || errno == ENOTCONN), "failed urgent send remains unconnected");
  require(sendto(client, "!", 1, MSG_NOSIGNAL | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && (errno == EPIPE || errno == ENOTCONN), "failed stream sendto cannot establish a transport");
  const int shutdown_rc = shutdown(client, SHUT_RDWR);
  require(shutdown_rc == -1 && errno == ENOTCONN, "failed stream shutdown remains unconnected");
  error = -1; length = sizeof(error);
  require(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "unconnected operations do not create an independent asynchronous error");
  consumed = {client, POLLPRI, 0};
  require(poll(&consumed, 1, 0) == 1 && consumed.revents == POLLHUP, "post-refusal operations retain only terminal hangup");
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

struct RequesterConnect { int fd; sockaddr_in address; int result = -99, error = -99; };
static void* requester_connect(void* raw) {
  auto& request = *static_cast<RequesterConnect*>(raw);
  request.result = connect(request.fd, reinterpret_cast<sockaddr*>(&request.address), sizeof(request.address));
  request.error = errno;
  return nullptr;
}
static void requester_exit_and_reuse() {
  int reserved = socket(AF_INET, SOCK_STREAM, 0); require(reserved >= 0, "requester reserve socket");
  RequesterConnect request{}; request.address.sin_family = AF_INET; request.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(reserved, reinterpret_cast<sockaddr*>(&request.address), sizeof(request.address)) == 0, "requester reserve bind");
  socklen_t length = sizeof(request.address);
  require(getsockname(reserved, reinterpret_cast<sockaddr*>(&request.address), &length) == 0, "requester reserve address");
  request.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); require(request.fd >= 0, "requester client");
  int alias = dup(request.fd); require(alias >= 0, "requester alias");
  pthread_t requester;
  require(pthread_create(&requester, nullptr, requester_connect, &request) == 0 && pthread_join(requester, nullptr) == 0, "actual connect requester exits");
  require(request.result == -1 && (request.error == EINPROGRESS || request.error == ECONNREFUSED), "requester result retained after exit");
  require(close(request.fd) == 0, "requester original descriptor close");
  pollfd p{alias, POLLPRI, 0};
  require(poll(&p, 1, 5000) == 1 && (p.revents & POLLERR) && !(p.revents & POLLPRI), "requester exit does not cancel retained alias refusal");
  int error = -1; length = sizeof(error);
  require(getsockopt(alias, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == ECONNREFUSED, "requester exit keeps exact refusal");
  require(close(alias) == 0, "requester last alias close");

  request.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); require(request.fd >= 0, "pending close client");
  request.result = connect(request.fd, reinterpret_cast<sockaddr*>(&request.address), sizeof(request.address));
  require(request.result == -1 && (errno == EINPROGRESS || errno == ECONNREFUSED), "pending close actual connect");
#ifdef GESHER
  gesher::SocketFd old;
  require(gesher::LookupSocket(request.fd, &old), "pending close original description");
  std::shared_ptr<gesher::ws::ConnectOperation> old_operation;
  { std::lock_guard<std::mutex> lock(old.description->error_mutex); old_operation = old.description->connect_operation; }
  require(old_operation != nullptr, "actual owner operation retained before last close");
#endif
  const int reused = request.fd;
  require(close(reused) == 0, "last close while connection operation is retained");
  int replacement = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); require(replacement >= 0, "replacement socket");
  if (replacement != reused) require(dup2(replacement, reused) == reused && close(replacement) == 0, "force exact fd reuse");
#ifdef GESHER
  gesher::SocketFd current;
  require(gesher::LookupSocket(reused, &current) && current.description != old.description, "fd reuse publishes a distinct description");
  const long long deadline = millis() + 2000;
  bool drained = false;
  do {
    { std::lock_guard<std::mutex> lock(old.description->error_mutex);
      drained = old.description->native_closed && !old.description->connect_operation; }
    if (!drained) usleep(1000);
  } while (!drained && millis() < deadline);
  require(drained, "last close observes actual old operation drain");
  require(gesher::ws::AwaitConnectRetirement(old_operation, 2000) == 0, "owning pthread and native reaper really exited after last close");
  require(current.description->state.load() == gesher::ws::ConnectState::kInitial, "old completion cannot mutate replacement generation");
#endif
  error = -1; length = sizeof(error);
  require(getsockopt(reused, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "replacement has no old connection error");
  p = {reused, POLLPRI, 0};
  require(poll(&p, 1, 0) == 1 && p.revents == POLLHUP, "replacement retains independent initial lifecycle");
  require(close(reused) == 0 && close(reserved) == 0, "requester and reuse cleanup");
  puts("PASS TCP priority: requester exit retains completion and last-close drains before fd generation reuse");
}

static void established_error_control() {
  int listener = socket(AF_INET, SOCK_STREAM, 0); require(listener >= 0, "reset listener");
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0, "reset bind/listen");
  socklen_t length = sizeof(address);
  require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "reset address");
  int client = socket(AF_INET, SOCK_STREAM, 0); require(client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "actual reset connection");
  int peer = accept(listener, nullptr, nullptr); require(peer >= 0, "reset peer");
  int alias = dup(client); require(alias >= 0 && close(client) == 0, "reset surviving alias");
  linger abortive{1, 0}; require(setsockopt(peer, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive)) == 0 && close(peer) == 0, "actual peer reset");
  pollfd p{alias, POLLIN | POLLPRI, 0};
  require(poll(&p, 1, 2000) == 1 && (p.revents & POLLERR) && !(p.revents & POLLPRI), "established reset error never masked as a consumed refusal");
  char byte{};
  // This is the actual caller consuming its receive error, never a readiness
  // implementation peeking/reading guest data or probing SO_ERROR.
  require(recv(alias, &byte, 1, MSG_DONTWAIT) == -1 && errno == ECONNRESET, "actual caller receives independent reset");
  int error = -1; length = sizeof(error);
  require(getsockopt(alias, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "caller reset receive consumed pending error");
  require(close(alias) == 0 && close(listener) == 0, "reset control cleanup");
  puts("PASS TCP priority: independent established reset remains poll-visible and reaches the caller receive");
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
  connect_finalization_window();
  requester_exit_and_reuse();
  established_error_control();
#ifdef GESHER
  owned_wait_controls();
  poll_close_reuse_controls();
  poll_native_error_controls();
  legacy_lifetime_controls();
  native_udp_error_control();
#endif
  failed_to_listener_control();
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
