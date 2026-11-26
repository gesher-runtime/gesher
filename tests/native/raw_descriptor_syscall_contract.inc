// Run on both Android guest ABIs and with the native Android/Linux kernel.
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>

static void test_raw_descriptor_syscalls(long (*raw2)(long, long, long)) {
  const int event_flags = EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE;
  const uint64_t cookie = UINT64_C(0x8ec2972a50134e11);
  int ep = (int)raw2(SYS_epoll_create1, EPOLL_CLOEXEC, -1);
  int event = (int)raw2(SYS_eventfd2, 0, event_flags);
  check("raw epoll_create1 creates a descriptor", ep >= 0);
  check("raw eventfd2 creates a descriptor", event >= 0);
  check("raw epoll_create1 rejects unsupported flags",
        raw2(SYS_epoll_create1, 1, 0) == -EINVAL);
  check("raw eventfd2 rejects unsupported flags",
        raw2(SYS_eventfd2, 0, -1) == -EINVAL);
  check("named epoll_create1 sets errno on failure",
        syscall(SYS_epoll_create1, 1) == -1 && errno == EINVAL);
  int named = (int)syscall(SYS_eventfd2, 2, event_flags);
  check("named eventfd2 creates a semaphore counter", named >= 0);
  if (named >= 0) {
    eventfd_t value = 0;
    struct pollfd ready = {.fd = named, .events = POLLIN};
    check("named eventfd2 forwards the initial value and semaphore flag",
          poll(&ready, 1, 0) == 1 && ready.revents == POLLIN &&
          eventfd_read(named, &value) == 0 && value == 1 &&
          poll(&ready, 1, 0) == 1 && ready.revents == POLLIN &&
          eventfd_read(named, &value) == 0 && value == 1);
    close(named);
  }
  if (ep >= 0 && event >= 0) {
    struct epoll_event request = {.events = EPOLLIN, .data.u64 = cookie};
    struct epoll_event result = {0};
    eventfd_t value = 0;
    check("raw descriptors accept a real epoll registration",
          epoll_ctl(ep, EPOLL_CTL_ADD, event, &request) == 0);
    int actual_flags = fcntl(event, F_GETFL);
    check("raw eventfd2 forwards nonblocking and reports empty",
          actual_flags >= 0 && (actual_flags & O_NONBLOCK) != 0 &&
          eventfd_read(event, &value) == -1 && errno == EAGAIN &&
          epoll_wait(ep, &result, 1, 0) == 0);
    check("eventfd write reaches raw epoll and preserves the data cookie",
          eventfd_write(event, 1) == 0 && epoll_wait(ep, &result, 1, 0) == 1 &&
          result.events == EPOLLIN && result.data.u64 == cookie);
    check("reading the raw eventfd clears epoll readiness",
          eventfd_read(event, &value) == 0 && value == 1 &&
          epoll_wait(ep, &result, 1, 0) == 0);
  }
  if (event >= 0) close(event);
  if (ep >= 0) close(ep);

#if defined(__x86_64__)
  errno = EDOM;
  check("raw legacy epoll_create rejects a nonpositive size",
        raw2(SYS_epoll_create, 0, -1) == -EINVAL && errno == EDOM);
  ep = (int)raw2(SYS_epoll_create, 1, -1);
  struct epoll_event result = {0};
  check("raw legacy epoll_create creates a usable epoll",
        ep >= 0 && epoll_wait(ep, &result, 1, 0) == 0);
  if (ep >= 0) close(ep);
  check("named legacy epoll_create sets errno on failure",
        syscall(SYS_epoll_create, 0) == -1 && errno == EINVAL);

  // The legacy inotify call has no arguments. Garbage register values cannot
  // become flags.
  int inotify_fd = (int)raw2(SYS_inotify_init, -1, -1);
  int named_inotify = (int)syscall(SYS_inotify_init);
  check("legacy inotify creation works through both entry points",
        inotify_fd >= 0 && named_inotify >= 0);
  if (inotify_fd >= 0) {
    int watch = inotify_add_watch(inotify_fd, ".", IN_CREATE);
    check("raw legacy inotify accepts and removes a real watch",
          watch >= 0 && inotify_rm_watch(inotify_fd, watch) == 0);
    close(inotify_fd);
  }
  if (named_inotify >= 0) close(named_inotify);
#endif
}
