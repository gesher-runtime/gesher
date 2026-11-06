// Shared by the native Windows probe and both Android ELF ABI regressions.
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int nested_add(int ep, int fd, uint32_t mask, uint64_t cookie) {
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = mask;
  event.data.u64 = cookie;
  return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event);
}

struct nested_waiter {
  int ep, result, error;
  uint64_t cookie;
  pthread_barrier_t* start;
};

static void* nested_wait(void* arg) {
  struct nested_waiter* w = (struct nested_waiter*)arg;
  pthread_barrier_wait(w->start);
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  w->result = epoll_wait(w->ep, &event, 1, 1500);
  w->cookie = event.data.u64;
  return NULL;
}

static void* nested_opposing_add(void* arg) {
  struct nested_waiter* w = (struct nested_waiter*)arg;
  pthread_barrier_wait(w->start);
  w->result = nested_add(w->ep, (int)w->cookie, EPOLLIN, 1);
  w->error = errno;
  return NULL;
}

static void* nested_reverse_graph(void* arg) {
  struct nested_waiter* w = (struct nested_waiter*)arg;
  int other = (int)w->cookie;
  pthread_barrier_wait(w->start);
  w->result = 0;
  for (int i = 0; i < 1000; ++i) {
    if (epoll_ctl(w->ep, EPOLL_CTL_DEL, other, NULL) != 0 ||
        nested_add(other, w->ep, EPOLLIN, 1) != 0 ||
        epoll_ctl(other, EPOLL_CTL_DEL, w->ep, NULL) != 0 ||
        nested_add(w->ep, other, EPOLLIN, 1) != 0) { w->result = -1; break; }
  }
  return NULL;
}

static void test_epoll_nested(void) {
  int leaf = eventfd(0, EFD_NONBLOCK);
  int child = epoll_create1(0), outer = epoll_create1(0);
  check("nested epoll setup", leaf >= 0 && child >= 0 && outer >= 0);
  check("nested epoll add", nested_add(child, leaf, EPOLLIN | EPOLLONESHOT, 11) == 0 &&
                              nested_add(outer, child, EPOLLIN, 22) == 0);
  struct pollfd p = {outer, POLLIN | POLLOUT, 0};
  check("empty epoll is not readable or writable", poll(&p, 1, 0) == 0 && p.revents == 0);
  check("nested epoll cycle rejected", nested_add(child, outer, EPOLLIN, 33) == -1 && errno == ELOOP);
  int alias = dup(outer);
  check("epoll self alias rejected", nested_add(outer, alias, EPOLLIN, 44) == -1 && errno == EINVAL);
  close(alias);
  check("nested exclusive rejected", nested_add(outer, child, EPOLLIN | EPOLLEXCLUSIVE, 1) == -1 && errno == EINVAL);
  eventfd_write(leaf, 1);
  p.fd = child;
  check("poll epoll does not consume oneshot", poll(&p, 1, 0) == 1 && p.revents == POLLIN && poll(&p, 1, 0) == 1);
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  check("outer reports its own cookie", epoll_wait(outer, &event, 1, 0) == 1 && event.data.u64 == 22);
  check("inner retains its event", epoll_wait(child, &event, 1, 0) == 1 && event.data.u64 == 11);
  p.fd = outer;
  check("disarmed child no longer readable", poll(&p, 1, 0) == 0 && epoll_wait(outer, &event, 1, 0) == 0);
  event.events = EPOLLIN | EPOLLONESHOT;
  event.data.u64 = 12;
  check("nested rearm", epoll_ctl(child, EPOLL_CTL_MOD, leaf, &event) == 0 && poll(&p, 1, 0) == 1);
  check("nested rearm preserves new cookie", epoll_wait(child, &event, 1, 0) == 1 && event.data.u64 == 12);
  close(outer); close(child); close(leaf);

  // Adding above and below a preexisting chain observes the same depth bound.
  int chain[6];
  for (int i = 0; i < 6; ++i) chain[i] = epoll_create1(0);
  int chain_ok = 1;
  for (int i = 0; i < 4; ++i) chain_ok &= nested_add(chain[i], chain[i + 1], EPOLLIN, 1) == 0;
  check("five epoll instances accepted", chain_ok);
  check("depth limit checked below", nested_add(chain[4], chain[5], EPOLLIN, 1) == -1 && errno == ELOOP);
  check("depth limit checked above", nested_add(chain[5], chain[0], EPOLLIN, 1) == -1 && errno == ELOOP);
  for (int i = 0; i < 6; ++i) close(chain[i]);

  pthread_barrier_t start;
  pthread_barrier_init(&start, NULL, 3);
  int a = epoll_create1(0), b = epoll_create1(0);
  struct nested_waiter x = {a, -2, 0, (uint64_t)b, &start};
  struct nested_waiter y = {b, -2, 0, (uint64_t)a, &start};
  pthread_t tx, ty;
  int sx = pthread_create(&tx, NULL, nested_opposing_add, &x);
  int sy = pthread_create(&ty, NULL, nested_opposing_add, &y);
  // Thread creation is already a required libc contract; fail boundedly if
  // it cannot be met instead of waiting on an incomplete three-party barrier.
  if (sx != 0 || sy != 0) _exit(2);
  pthread_barrier_wait(&start);
  pthread_join(tx, NULL); pthread_join(ty, NULL);
  check("opposing adds cannot form a cycle", (x.result == 0 && y.result == -1 && y.error == ELOOP) ||
                                            (y.result == 0 && x.result == -1 && x.error == ELOOP));
  close(a); close(b);
  pthread_barrier_destroy(&start);

  // Unlocked recursive samples may observe different historical directions.
  // They must remain bounded even while every individual graph is acyclic.
  a = epoll_create1(0); b = epoll_create1(0);
  nested_add(a, b, EPOLLIN, 1);
  pthread_barrier_init(&start, NULL, 2);
  x = (struct nested_waiter){a, -2, 0, (uint64_t)b, &start};
  sx = pthread_create(&tx, NULL, nested_reverse_graph, &x);
  if (sx != 0) _exit(2);
  pthread_barrier_wait(&start);
  int sampled = 1;
  for (int i = 0; i < 1000; ++i) {
    struct pollfd changing[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
    sampled &= poll(changing, 2, 0) == 0;
  }
  pthread_join(tx, NULL);
  check("concurrent graph reversal sampling is bounded", sampled && x.result == 0);
  close(a); close(b);
  pthread_barrier_destroy(&start);

  // An ADD wakes every level-triggered waiter, even when the old list was empty.
  a = epoll_create1(0); leaf = eventfd(1, EFD_NONBLOCK);
  pthread_barrier_init(&start, NULL, 3);
  x = (struct nested_waiter){a, -2, 0, 0, &start};
  y = (struct nested_waiter){a, -2, 0, 0, &start};
  sx = pthread_create(&tx, NULL, nested_wait, &x);
  sy = pthread_create(&ty, NULL, nested_wait, &y);
  if (sx != 0 || sy != 0) _exit(2);
  pthread_barrier_wait(&start);
  usleep(50000);
  check("add to waiting epoll", nested_add(a, leaf, EPOLLIN, 77) == 0);
  pthread_join(tx, NULL); pthread_join(ty, NULL);
  check("all level waiters refresh their interest list", x.result == 1 && y.result == 1 && x.cookie == 77 && y.cookie == 77);
  close(a); close(leaf);
  pthread_barrier_destroy(&start);
}
