// Independent Linux-only oracle: real kernel backlog, never a mocked connect.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <atomic>

static void require(bool value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL LINUX_CONNECT_WAIT %s errno=%d\n", label, errno); exit(1); }
}
static long long millis() {
  timespec value{}; require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "monotonic clock");
  return static_cast<long long>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}
struct Attempt {
  int socket = -1; sockaddr_in address{};
  std::atomic<bool> returned{false}; std::atomic<int> result{-99}, error{-99};
};
static void* connecting(void* raw) {
  auto& attempt = *static_cast<Attempt*>(raw);
  attempt.result = connect(attempt.socket, reinterpret_cast<sockaddr*>(&attempt.address), sizeof(attempt.address));
  attempt.error = errno; attempt.returned.store(true, std::memory_order_release); return nullptr;
}
static int socket_error(int socket) {
  int value = -1; socklen_t length = sizeof(value);
  require(getsockopt(socket, SOL_SOCKET, SO_ERROR, &value, &length) == 0 && length == sizeof(value), "actual caller error read"); return value;
}
static bool pending(int socket) {
  sockaddr_in local{}; socklen_t length = sizeof(local);
  require(getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0, "actual local address");
  pollfd p{socket, POLLOUT, 0}; const int ready = poll(&p, 1, 0);
  require(ready >= 0, "actual pending readiness");
  return local.sin_port != 0 && ready == 0 && p.revents == 0;
}
static void scenario(int mode) {
  int listener = socket(AF_INET, SOCK_STREAM, 0); require(listener >= 0, "listener");
  Attempt attempt{}; attempt.address.sin_family = AF_INET; attempt.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(listener, reinterpret_cast<sockaddr*>(&attempt.address), sizeof(attempt.address)) == 0 && listen(listener, 0) == 0, "bounded zero-backlog listener");
  socklen_t length = sizeof(attempt.address); require(getsockname(listener, reinterpret_cast<sockaddr*>(&attempt.address), &length) == 0, "listener address");
  int filler = socket(AF_INET, SOCK_STREAM, 0); require(filler >= 0 && connect(filler, reinterpret_cast<sockaddr*>(&attempt.address), sizeof(attempt.address)) == 0, "actual accept-queue filler");
  attempt.socket = socket(AF_INET, SOCK_STREAM | (mode == 3 ? SOCK_NONBLOCK : 0), 0); require(attempt.socket >= 0, "client");
  const long long start = millis(); pthread_t caller{}; bool threaded = mode == 1 || mode == 2;
  if (mode == 0) {
    timeval timeout{0, 60000}; require(setsockopt(attempt.socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "send deadline");
    connecting(&attempt);
    require(attempt.result == -1 && attempt.error == EINPROGRESS && millis() - start >= 40 && millis() - start < 1500, "send timeout returns pending without abort");
  } else if (threaded) require(pthread_create(&caller, nullptr, connecting, &attempt) == 0, "real blocking connector");
  else { connecting(&attempt); require(attempt.result == -1 && attempt.error == EINPROGRESS, "initial nonblocking connect result"); }
  const long long pending_deadline = millis() + 2000;
  while (!pending(attempt.socket) && millis() < pending_deadline) usleep(1000);
  require(pending(attempt.socket) && (!threaded || !attempt.returned.load(std::memory_order_acquire)), "actual kernel pending connection precondition");
  int canceled = 0;
  if (mode == 1) {
    require(pthread_cancel(caller) == 0, "cancel actual blocked caller"); void* value = nullptr;
    require(pthread_join(caller, &value) == 0 && value == PTHREAD_CANCELED, "actual canceled caller exit"); canceled = 1;
    require(pending(attempt.socket), "canceling caller preserves kernel connection attempt");
  } else if (mode == 2 || mode == 3) {
    const int flags = fcntl(attempt.socket, F_GETFL); require(flags >= 0, "actual status flags");
    require(fcntl(attempt.socket, F_SETFL, mode == 2 ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0, "mode changes during actual pending operation");
    require(!threaded || !attempt.returned.load(std::memory_order_acquire), "blocking caller does not become nonblocking mid-call");
  }
  const int before = socket_error(attempt.socket);
  printf("LINUX_CONNECT_WAIT mode=%d phase=still-pending connect_rc=%d connect_errno=%d canceled=%d elapsed_ms=%lld error=%d\n", mode, attempt.result.load(), attempt.error.load(), canceled, millis() - start, before); fflush(stdout);
  require(before == 0 && pending(attempt.socket), "caller wait interruption does not consume or abort transport");
  int accepted_filler = accept(listener, nullptr, nullptr); require(accepted_filler >= 0, "release real accept queue");
  pollfd p{attempt.socket, POLLOUT, 0}; const int ready = poll(&p, 1, 2000); const int completion = socket_error(attempt.socket);
  printf("LINUX_CONNECT_WAIT mode=%d phase=late-completion poll=%d revents=%d error=%d elapsed_ms=%lld\n", mode, ready, p.revents, completion, millis() - start); fflush(stdout);
  require(ready == 1 && (p.revents & POLLOUT) && !(p.revents & POLLERR) && completion == 0, "same connection completes after wait ends or mode changes");
  if (mode == 2) require(pthread_join(caller, nullptr) == 0 && attempt.result == 0, "original blocking caller completes despite new nonblocking flag");
  p = {listener, POLLIN, 0}; require(poll(&p, 1, 2000) == 1, "late accepted peer available");
  int peer = accept(listener, nullptr, nullptr); require(peer >= 0 && send(peer, "v", 1, MSG_NOSIGNAL) == 1, "same late transport carries payload");
  p = {attempt.socket, POLLIN, 0}; require(poll(&p, 1, 2000) == 1, "late payload arrival");
  char byte{}; require(recv(attempt.socket, &byte, 1, 0) == 1 && byte == 'v', "late actual payload");
  require(close(peer) == 0 && close(accepted_filler) == 0 && close(filler) == 0 && close(attempt.socket) == 0 && close(listener) == 0, "all kernel resources closed");
}
int main() {
  for (int mode = 0; mode != 4; ++mode) scenario(mode);
  puts("PASS independent Linux actual connect send timeout caller cancellation both mode directions and late transport completion");
}
