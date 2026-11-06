// A native-network-worker regression: an eventfd starts work on a pthread,
// then the same epoll watches that eventfd and a nonblocking TCP connection.
// A loopback peer exchanges a SOCKS5 greeting/connect reply and synthetic data.
// No real proxy, server, credentials or Telegram account is involved.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct tgloop_tag { int kind; };
struct tgloop_context {
  int listener, wake;
  uint16_t port;
  int server_ok, worker_ok, wake_seen, cookies_ok;
  int64_t deadline;
};

static int64_t tgloop_now(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int tgloop_left(int64_t deadline) {
  int64_t left = deadline - tgloop_now();
  return left > 0 ? (left > 5000 ? 5000 : (int)left) : 0;
}

static int tgloop_peer_wait(int fd, short events, int64_t deadline) {
  while (tgloop_left(deadline) > 0) {
    struct pollfd p = {fd, events, 0};
    int n = poll(&p, 1, tgloop_left(deadline));
    if (n > 0) return (p.revents & (events | POLLERR | POLLHUP)) != 0;
    if (n < 0 && errno != EINTR) return 0;
  }
  return 0;
}

static int tgloop_peer_io(int fd, void* data, size_t size, int writing, int64_t deadline) {
  size_t offset = 0;
  while (offset < size && tgloop_peer_wait(fd, writing ? POLLOUT : POLLIN, deadline)) {
    ssize_t n = writing ? send(fd, (char*)data + offset, size - offset, MSG_NOSIGNAL)
                        : recv(fd, (char*)data + offset, size - offset, 0);
    if (n > 0) offset += (size_t)n;
    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return 0;
  }
  return offset == size;
}

static void* tgloop_peer(void* raw) {
  struct tgloop_context* c = raw;
  int fd = -1;
  while (tgloop_peer_wait(c->listener, POLLIN, c->deadline)) {
    fd = accept4(c->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
  }
  if (fd < 0) return NULL;
  unsigned char greeting[3], request[10], data[4];
  unsigned char method[] = {5, 0};
  unsigned char connected[] = {5, 0, 0, 1, 127, 0, 0, 1, 0, 0};
  c->server_ok = tgloop_peer_io(fd, greeting, sizeof(greeting), 0, c->deadline)
      && memcmp(greeting, "\x05\x01\x00", sizeof(greeting)) == 0
      && tgloop_peer_io(fd, method, sizeof(method), 1, c->deadline)
      && tgloop_peer_io(fd, request, sizeof(request), 0, c->deadline)
      && memcmp(request, "\x05\x01\x00\x01\x7f\x00\x00\x01\x01\xbb", sizeof(request)) == 0
      && tgloop_peer_io(fd, connected, sizeof(connected), 1, c->deadline)
      && tgloop_peer_io(fd, data, sizeof(data), 0, c->deadline)
      && memcmp(data, "PING", sizeof(data)) == 0
      && tgloop_peer_io(fd, (void*)"PONG", 4, 1, c->deadline);
  close(fd);
  return NULL;
}

static int tgloop_wait_socket(struct tgloop_context* c, int ep,
    struct tgloop_tag* wake_tag, struct tgloop_tag* socket_tag, uint32_t wanted) {
  while (tgloop_left(c->deadline) > 0) {
    struct epoll_event events[8];
    int n = epoll_wait(ep, events, 8, tgloop_left(c->deadline));
    if (n < 0) { if (errno == EINTR) continue; return 0; }
    int ready = 0;
    for (int i = 0; i < n; ++i) {
      if (events[i].data.ptr == wake_tag) {
        eventfd_t value;
        if (eventfd_read(c->wake, &value) == 0) c->wake_seen += value == 1;
        else if (errno != EAGAIN) return 0;
      } else if (events[i].data.ptr == socket_tag) {
        ready |= (events[i].events & (wanted | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
      } else {
        c->cookies_ok = 0;
        return 0;
      }
    }
    if (ready) return 1;
  }
  return 0;
}

static int tgloop_worker_io(struct tgloop_context* c, int ep, int fd,
    struct tgloop_tag* wake_tag, struct tgloop_tag* socket_tag, void* data,
    size_t size, int writing) {
  struct epoll_event event = {0};
  event.data.ptr = socket_tag;
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLET | (writing ? EPOLLOUT : 0);
  if (epoll_ctl(ep, EPOLL_CTL_MOD, fd, &event) != 0) return 0;
  size_t offset = 0;
  while (offset < size && tgloop_wait_socket(c, ep, wake_tag, socket_tag,
                                            writing ? EPOLLOUT : EPOLLIN)) {
    ssize_t n = writing ? send(fd, (char*)data + offset, size - offset, MSG_NOSIGNAL)
                        : recv(fd, (char*)data + offset, size - offset, 0);
    if (n > 0) offset += (size_t)n;
    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return 0;
  }
  return offset == size;
}

static void* tgloop_worker(void* raw) {
  struct tgloop_context* c = raw;
  struct tgloop_tag wake_tag = {1}, socket_tag = {2};
  int ep = epoll_create(128), fd = -1;
  struct epoll_event event = {0};
  if (ep < 0) return NULL;
  event.events = EPOLLIN | EPOLLET;
  event.data.ptr = &wake_tag;
  if (epoll_ctl(ep, EPOLL_CTL_ADD, c->wake, &event) != 0) goto done;
  // There is no socket yet: a producer's eventfd write must wake this wait.
  while (!c->wake_seen && tgloop_left(c->deadline) > 0) {
    struct epoll_event ready;
    int n = epoll_wait(ep, &ready, 1, tgloop_left(c->deadline));
    if (n < 0 && errno == EINTR) continue;
    if (n != 1 || ready.data.ptr != &wake_tag || !(ready.events & EPOLLIN)) goto done;
    eventfd_t value;
    if (eventfd_read(c->wake, &value) != 0 || value != 1) goto done;
    c->wake_seen = 1;
  }
  if (!c->wake_seen) goto done;
  fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) goto done;
  struct sockaddr_in to = {0};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  to.sin_port = htons(c->port);
  int connected = connect(fd, (const struct sockaddr*)&to, sizeof(to));
  if (connected < 0 && errno != EINPROGRESS) goto done;
  event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
  event.data.ptr = &socket_tag;
  if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) != 0
      || !tgloop_wait_socket(c, ep, &wake_tag, &socket_tag, EPOLLOUT)) goto done;
  int error = -1;
  socklen_t error_size = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error != 0) goto done;
  unsigned char greeting[] = {5, 1, 0}, method[2];
  unsigned char request[] = {5, 1, 0, 1, 127, 0, 0, 1, 1, 187}, reply[10];
  unsigned char data[4];
  c->worker_ok = tgloop_worker_io(c, ep, fd, &wake_tag, &socket_tag, greeting, sizeof(greeting), 1)
      && tgloop_worker_io(c, ep, fd, &wake_tag, &socket_tag, method, sizeof(method), 0)
      && memcmp(method, "\x05\x00", sizeof(method)) == 0
      && tgloop_worker_io(c, ep, fd, &wake_tag, &socket_tag, request, sizeof(request), 1)
      && tgloop_worker_io(c, ep, fd, &wake_tag, &socket_tag, reply, sizeof(reply), 0)
      && memcmp(reply, "\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", sizeof(reply)) == 0
      && tgloop_worker_io(c, ep, fd, &wake_tag, &socket_tag, (void*)"PING", 4, 1)
      && tgloop_worker_io(c, ep, fd, &wake_tag, &socket_tag, data, sizeof(data), 0)
      && memcmp(data, "PONG", sizeof(data)) == 0;
done:
  if (fd >= 0) close(fd);
  close(ep);
  return NULL;
}

static void test_tgnet_event_loop(void) {
  int completed = 0, woke = 0, cookies = 1;
  for (int cycle = 0; cycle < 4; ++cycle) {
    struct tgloop_context c = {0};
    c.cookies_ok = 1;
    c.deadline = tgloop_now() + 5000;
    c.listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    c.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (c.listener < 0 || c.wake < 0) {
      if (c.listener >= 0) close(c.listener);
      if (c.wake >= 0) close(c.wake);
      break;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (bind(c.listener, (const struct sockaddr*)&address, sizeof(address)) != 0
        || listen(c.listener, 1) != 0
        || getsockname(c.listener, (struct sockaddr*)&address, &size) != 0) {
      close(c.listener); close(c.wake); break;
    }
    c.port = ntohs(address.sin_port);
    pthread_t peer, worker;
    int peer_started = pthread_create(&peer, NULL, tgloop_peer, &c) == 0;
    int worker_started = peer_started && pthread_create(&worker, NULL, tgloop_worker, &c) == 0;
    if (worker_started) {
      eventfd_write(c.wake, 1);
      pthread_join(worker, NULL);
    }
    if (peer_started) pthread_join(peer, NULL);
    completed += worker_started && c.worker_ok && c.server_ok;
    woke += c.wake_seen == 1;
    cookies &= c.cookies_ok;
    close(c.listener);
    close(c.wake);
  }
  check("native eventfd wakes TCP worker (4 cycles)", woke == 4);
  check("native epoll preserves event data.ptr cookies", cookies && completed == 4);
  check("native nonblocking TCP/SOCKS-like exchange", completed == 4);
}
