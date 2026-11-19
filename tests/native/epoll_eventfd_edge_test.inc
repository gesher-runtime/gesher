/* Ordinary eventfd EPOLLET contracts shared by Linux, Windows and both guests.
 * check(label, condition) is supplied by the containing fixture. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static int edge_ctl(int ep, int op, int fd, uint32_t mask, uint64_t cookie) {
  struct epoll_event event;
  memset(&event, 0, sizeof(event)); event.events = mask; event.data.u64 = cookie;
  return epoll_ctl(ep, op, fd, &event);
}
static int edge_wait(int ep, uint32_t mask, uint64_t cookie, int timeout) {
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  return epoll_wait(ep, &event, 1, timeout) == 1 && event.events == mask && event.data.u64 == cookie;
}
static int edge_empty(int ep, int timeout) {
  struct epoll_event event;
  return epoll_wait(ep, &event, 1, timeout) == 0;
}
static int64_t edge_millis(void) {
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void* edge_write_later(void* raw) {
  int fd = *(int*)raw; usleep(50000);
  return eventfd_write(fd, 1) == 0 ? NULL : raw;
}
static unsigned edge_two(int ep, uint64_t first) {
  unsigned seen = 0;
  for (int i = 0; i < 2; ++i) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    if (epoll_wait(ep, &event, 1, 0) == 1 && event.events == EPOLLIN &&
        event.data.u64 >= first && event.data.u64 <= first + 1)
      seen |= 1u << (unsigned)(event.data.u64 - first);
  }
  return seen;
}

static void test_epoll_eventfd_edges(void) {
  int ep = epoll_create1(0), fd = eventfd(2, EFD_NONBLOCK | EFD_SEMAPHORE);
  eventfd_t value = 0;
  check("eventfd edge setup", ep >= 0 && fd >= 0);
  if (ep >= 0 && fd >= 0) {
    check("eventfd edge ADD", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 11) == 0);
    check("eventfd edge initial readable", edge_wait(ep, EPOLLIN, 11, 0));
    check("eventfd edge undrained is not repeated", edge_empty(ep, 0));
    check("eventfd semaphore partial read", eventfd_read(fd, &value) == 0 && value == 1);
    check("eventfd edge partial drain stays suppressed", edge_empty(ep, 0));
    check("eventfd semaphore full drain", eventfd_read(fd, &value) == 0 && value == 1);
    check("eventfd refill between waits", eventfd_write(fd, 1) == 0 && edge_wait(ep, EPOLLIN, 11, 0));
    check("eventfd refilled edge consumed", edge_empty(ep, 0));
    check("eventfd write while already readable", eventfd_write(fd, 1) == 0 && edge_wait(ep, EPOLLIN, 11, 0));
    check("eventfd repeated producer edge consumed", edge_empty(ep, 0));
    check("eventfd zero write publishes readable notification", eventfd_write(fd, 0) == 0 && edge_wait(ep, EPOLLIN, 11, 0));
    check("eventfd zero-write edge consumed", edge_empty(ep, 0));
    check("eventfd failed write", eventfd_write(fd, UINT64_MAX) == -1 && errno == EINVAL);
    check("eventfd failed write does not notify", edge_empty(ep, 0));
    check("eventfd edge MOD", edge_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET, 12) == 0);
    check("eventfd MOD resamples and changes cookie", edge_wait(ep, EPOLLIN, 12, 0));
    check("eventfd MOD edge consumed", edge_empty(ep, 0));
    check("eventfd edge oneshot MOD", edge_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET | EPOLLONESHOT, 13) == 0);
    check("eventfd edge oneshot delivery", edge_wait(ep, EPOLLIN, 13, 0));
    check("eventfd oneshot remains disarmed", eventfd_write(fd, 1) == 0 && edge_empty(ep, 0));
    check("eventfd oneshot rearm", edge_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET | EPOLLONESHOT, 14) == 0);
    check("eventfd oneshot new cookie", edge_wait(ep, EPOLLIN, 14, 0) && edge_empty(ep, 0));
    check("eventfd edge DEL", epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == 0);
    check("eventfd removed entry stays absent", eventfd_write(fd, 1) == 0 && edge_empty(ep, 0));
    check("eventfd re-add initial notification", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 15) == 0 && edge_wait(ep, EPOLLIN, 15, 0) && edge_empty(ep, 0));
    int alias = dup(ep);
    check("eventfd epoll alias shares consumed edge", alias >= 0 && edge_empty(alias, 0));
    check("eventfd epoll alias receives new edge", eventfd_write(fd, 1) == 0 && edge_wait(alias, EPOLLIN, 15, 0));
    check("eventfd epoll original sees alias consumption", edge_empty(ep, 0));
    if (alias >= 0) close(alias);
    int64_t start = edge_millis();
    int empty = edge_empty(ep, 30);
    check("eventfd consumed edge finite timeout", empty && edge_millis() - start >= 15);
    pthread_t writer;
    start = edge_millis();
    int created = pthread_create(&writer, NULL, edge_write_later, &fd) == 0;
    check("eventfd delayed producer starts", created);
    if (created) {
      void* result = &fd;
      int delivered = edge_wait(ep, EPOLLIN, 15, 1000);
      check("eventfd consumed readiness blocks until producer", delivered && edge_millis() - start >= 20);
      pthread_join(writer, &result);
      check("eventfd delayed producer succeeded", result == NULL && edge_empty(ep, 0));
    }
  }
  if (fd >= 0) close(fd);
  if (ep >= 0) close(ep);

  ep = epoll_create1(0); fd = eventfd(1, EFD_NONBLOCK);
  check("eventfd output setup", ep >= 0 && fd >= 0);
  if (ep >= 0 && fd >= 0) {
    check("eventfd output initial edge", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLOUT | EPOLLET, 21) == 0 && edge_wait(ep, EPOLLOUT, 21, 0));
    check("eventfd output level suppressed", edge_empty(ep, 0));
    check("eventfd input does not notify output-only interest", eventfd_write(fd, 1) == 0 && edge_empty(ep, 0));
    check("eventfd read notifies writable interest", eventfd_read(fd, &value) == 0 && value == 2 && edge_wait(ep, EPOLLOUT, 21, 0));
    check("eventfd output notification consumed", edge_empty(ep, 0));
    check("eventfd full counter not writable", eventfd_write(fd, UINT64_MAX - 1) == 0 && edge_empty(ep, 0));
    check("eventfd full write fails", eventfd_write(fd, 1) == -1 && errno == EAGAIN && edge_empty(ep, 0));
    check("eventfd read restores writable notification", eventfd_read(fd, &value) == 0 && value == UINT64_MAX - 1 && edge_wait(ep, EPOLLOUT, 21, 0));
    check("eventfd restored output consumed", edge_empty(ep, 0));
    check("eventfd empty zero-write unreadable", edge_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET, 22) == 0 && eventfd_write(fd, 0) == 0 && edge_empty(ep, 0));
    check("eventfd write after empty zero-write", eventfd_write(fd, 1) == 0 && edge_wait(ep, EPOLLIN, 22, 0) && edge_empty(ep, 0));
    check("eventfd combined interest MOD", edge_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLOUT | EPOLLET, 23) == 0 && edge_wait(ep, EPOLLIN | EPOLLOUT, 23, 0));
    check("eventfd notification reports all ready requested bits", eventfd_write(fd, 1) == 0 && edge_wait(ep, EPOLLIN | EPOLLOUT, 23, 0) && edge_empty(ep, 0));
  }
  if (fd >= 0) close(fd);
  if (ep >= 0) close(ep);

  ep = epoll_create1(0); fd = eventfd(1, EFD_NONBLOCK);
  {
    int other = epoll_create1(0), alias = dup(fd);
    check("eventfd independent registrations setup", ep >= 0 && fd >= 0 && other >= 0 && alias >= 0);
    if (ep >= 0 && fd >= 0 && other >= 0 && alias >= 0) {
      check("eventfd duplicate registrations add", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 31) == 0 && edge_ctl(ep, EPOLL_CTL_ADD, alias, EPOLLIN | EPOLLET, 32) == 0 && edge_ctl(other, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 33) == 0);
      check("eventfd duplicate maxevents progress", edge_two(ep, 31) == 3 && edge_empty(ep, 0));
      check("eventfd separate epoll retains edge", edge_wait(other, EPOLLIN, 33, 0) && edge_empty(other, 0));
      check("eventfd write through duplicate reaches both epolls", eventfd_write(alias, 1) == 0 && edge_wait(other, EPOLLIN, 33, 0));
      check("eventfd producer reaches both duplicate registrations", edge_two(ep, 31) == 3 && edge_empty(ep, 0) && edge_empty(other, 0));
    }
    if (other >= 0) close(other);
    if (alias >= 0) close(alias);
  }
  if (fd >= 0) close(fd);
  if (ep >= 0) close(ep);

  ep = epoll_create1(0); fd = eventfd(1, EFD_NONBLOCK);
  {
    int outer = epoll_create1(0);
    check("eventfd nested edge setup", ep >= 0 && fd >= 0 && outer >= 0);
    if (ep >= 0 && fd >= 0 && outer >= 0) {
      check("eventfd nested edge add", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 41) == 0 && edge_ctl(outer, EPOLL_CTL_ADD, ep, EPOLLIN, 42) == 0);
      struct pollfd p = {ep, POLLIN, 0};
      check("eventfd epoll readability does not consume", poll(&p, 1, 0) == 1 && p.revents == POLLIN && poll(&p, 1, 0) == 1);
      check("eventfd outer level reports queued edge", edge_wait(outer, EPOLLIN, 42, 0) && edge_wait(outer, EPOLLIN, 42, 0));
      check("eventfd inner edge delivered", edge_wait(ep, EPOLLIN, 41, 0));
      check("eventfd consumed edge makes epoll unreadable", poll(&p, 1, 0) == 0 && edge_empty(outer, 0));
      check("eventfd producer restores nested readability", eventfd_write(fd, 1) == 0 && edge_wait(outer, EPOLLIN, 42, 0) && edge_wait(ep, EPOLLIN, 41, 0) && edge_empty(outer, 0));
    }
    if (outer >= 0) close(outer);
  }
  if (fd >= 0) close(fd);
  if (ep >= 0) close(ep);

  ep = epoll_create1(0); fd = eventfd(1, EFD_NONBLOCK);
  {
    int level = eventfd(1, EFD_NONBLOCK);
    check("eventfd mixed level setup", ep >= 0 && fd >= 0 && level >= 0);
    if (ep >= 0 && fd >= 0 && level >= 0) {
      check("eventfd mixed edge initial", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 51) == 0 && edge_wait(ep, EPOLLIN, 51, 0));
      check("eventfd level registration", edge_ctl(ep, EPOLL_CTL_ADD, level, EPOLLIN, 52) == 0);
      check("eventfd suppressed edge does not hide level", edge_wait(ep, EPOLLIN, 52, 0) && edge_wait(ep, EPOLLIN, 52, 0));
      eventfd_write(fd, 1);
      check("eventfd mixed round-robin progress", edge_two(ep, 51) == 3);
      check("eventfd edge retirement preserves level", edge_wait(ep, EPOLLIN, 52, 0));
      close(level); level = -1; close(fd); fd = -1;
      fd = eventfd(1, EFD_NONBLOCK);
      check("eventfd numeric reuse drops old registration", fd >= 0 && edge_empty(ep, 0));
      check("eventfd reused number fresh cookie", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 53) == 0 && edge_wait(ep, EPOLLIN, 53, 0) && edge_empty(ep, 0));
    }
    if (level >= 0) close(level);
  }
  if (fd >= 0) close(fd);
  if (ep >= 0) close(ep);
  // Unlike sockets, Linux eventfd does not expose the normal-band aliases.
  const uint32_t masks[] = {EPOLLRDNORM, EPOLLWRNORM, EPOLLRDNORM | EPOLLWRNORM,
      EPOLLIN | EPOLLRDNORM, EPOLLOUT | EPOLLWRNORM,
      EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM};
  for (int level = 0; level <= 1; ++level) {
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
      ep = epoll_create1(0); fd = eventfd(1, EFD_NONBLOCK);
      char label[96];
      snprintf(label, sizeof(label), "eventfd %s mask %#x setup", level ? "LT" : "ET", masks[i]);
      check(label, ep >= 0 && fd >= 0 && edge_ctl(ep, EPOLL_CTL_ADD, fd, masks[i] | (level ? 0u : (uint32_t)EPOLLET), 61) == 0);
      if (ep >= 0 && fd >= 0) {
        uint32_t canonical = masks[i] & (EPOLLIN | EPOLLOUT);
        snprintf(label, sizeof(label), "eventfd %s mask %#x ADD canonical bits", level ? "LT" : "ET", masks[i]);
        check(label, canonical ? edge_wait(ep, canonical, 61, 0) : edge_empty(ep, 0));
        snprintf(label, sizeof(label), "eventfd %s mask %#x write canonical bits", level ? "LT" : "ET", masks[i]);
        check(label, eventfd_write(fd, 1) == 0 &&
              ((level ? canonical != 0 : (canonical & EPOLLIN) != 0) ? edge_wait(ep, canonical, 61, 0) : edge_empty(ep, 0)));
        snprintf(label, sizeof(label), "eventfd %s mask %#x read canonical bits", level ? "LT" : "ET", masks[i]);
        check(label, eventfd_read(fd, &value) == 0 && value == 2 &&
              ((canonical & EPOLLOUT) ? edge_wait(ep, EPOLLOUT, 61, 0) : edge_empty(ep, 0)));
      }
      if (fd >= 0) close(fd);
      if (ep >= 0) close(ep);
    }
  }
  printf("eventfd edge contracts: complete\n");
}
