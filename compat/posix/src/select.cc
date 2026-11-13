// gesher-base: select and pselect (<sys/select.h>) through poll, so they wait
// on every kind of descriptor poll takes (src/fd.cc). As on Linux, a
// descriptor that is not open fails the call with EBADF, the sets hold only
// the ready descriptors on return, and the result counts set bits.
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/select.h>
#include <windows.h>

#include <limits>
#include <vector>

#include "internal.h"

namespace {

// Nanoseconds in a wide signed type retain all accepted timespec ranges and
// cannot overflow when adding the host's monotonic clock to the duration.
using WideTime = __int128;

WideTime NowNs() { return static_cast<WideTime>(gesher::MonotonicMicros()) * 1000; }

int SelectUntil(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                bool finite, WideTime deadline) {
  if (nfds < 0 || nfds > FD_SETSIZE) return errno = EINVAL, -1;
  std::vector<pollfd> fds;
  for (int fd = 0; fd < nfds; ++fd) {
    short events = 0;
    if (readfds != nullptr && FD_ISSET(fd, readfds)) events |= POLLIN;
    if (writefds != nullptr && FD_ISSET(fd, writefds)) events |= POLLOUT;
    if (exceptfds != nullptr && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
    if (events != 0) fds.push_back({fd, events, 0});
  }
  for (;;) {
    int timeout_ms = -1;
    if (finite) {
      const WideTime remaining = deadline - NowNs();
      const WideTime milliseconds = remaining > 0 ? (remaining + 999999) / 1000000 : 0;
      timeout_ms = milliseconds > INT_MAX ? INT_MAX : static_cast<int>(milliseconds);
    }
    // poll also handles an empty interest set. Keep the same absolute deadline
    // through bounded slices and through an early Windows scheduler wakeup.
    int ready = poll(fds.data(), fds.size(), timeout_ms);
    if (ready < 0) return -1;
    if (ready > 0 || (finite && NowNs() >= deadline)) break;
  }
  // With no requested descriptors, preserve the existing treatment of bits
  // outside nfds. They are not part of this wait's result sets.
  if (fds.empty()) return 0;
  for (const pollfd& p : fds) {
    if ((p.revents & POLLNVAL) != 0) return errno = EBADF, -1;
  }
  if (readfds != nullptr) FD_ZERO(readfds);
  if (writefds != nullptr) FD_ZERO(writefds);
  if (exceptfds != nullptr) FD_ZERO(exceptfds);
  int count = 0;
  for (const pollfd& p : fds) {
    // Linux reports a hung-up or failed descriptor as readable and writable,
    // where it was asked: the next read or write returns the condition.
    if (readfds != nullptr && (p.events & POLLIN) != 0 &&
        (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      FD_SET(p.fd, readfds);
      ++count;
    }
    if (writefds != nullptr && (p.events & POLLOUT) != 0 &&
        (p.revents & (POLLOUT | POLLERR)) != 0) {
      FD_SET(p.fd, writefds);
      ++count;
    }
    if (exceptfds != nullptr && (p.events & POLLPRI) != 0 && (p.revents & POLLPRI) != 0) {
      FD_SET(p.fd, exceptfds);
      ++count;
    }
  }
  return count;
}

}  // namespace

extern "C" int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                      struct timeval* timeout) {
  if (timeout != nullptr && (timeout->tv_sec < 0 || timeout->tv_usec < 0)) {
    return errno = EINVAL, -1;
  }
  const bool finite = timeout != nullptr;
  const WideTime duration = finite
      ? static_cast<WideTime>(timeout->tv_sec) * 1000000000 +
        static_cast<WideTime>(timeout->tv_usec) * 1000 : 0;
  const WideTime deadline = finite ? NowNs() + duration : 0;
  int result = SelectUntil(nfds, readfds, writefds, exceptfds, finite, deadline);
  // Linux reports the residual budget after readiness and interrupted/error
  // waits too. Keep the error from poll/descriptor validation unchanged.
  const int saved_errno = errno;
  if (finite) {
    WideTime remaining = deadline - NowNs();
    if (remaining < 0) remaining = 0;
    WideTime seconds = remaining / 1000000000;
    const WideTime max_seconds = std::numeric_limits<decltype(timeout->tv_sec)>::max();
    if (seconds > max_seconds) seconds = max_seconds;
    // timeval on Windows has 32-bit longs. Its accepted non-normalized usec
    // input can extend past LONG_MAX seconds; retain that representable form
    // instead of narrowing a normalized seconds field to a negative value.
    timeout->tv_sec = static_cast<decltype(timeout->tv_sec)>(seconds);
    timeout->tv_usec = static_cast<decltype(timeout->tv_usec)>((remaining - seconds * 1000000000) / 1000);
  }
  errno = saved_errno;
  return result;
}

// Signals are not delivered asynchronously here (as for ppoll), so the mask
// has nothing to change while waiting.
extern "C" int pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                       const struct timespec* timeout, const sigset_t*) {
  if (timeout != nullptr &&
      (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000)) {
    return errno = EINVAL, -1;
  }
  const bool finite = timeout != nullptr;
  const WideTime duration = finite
      ? static_cast<WideTime>(timeout->tv_sec) * 1000000000 + timeout->tv_nsec : 0;
  const WideTime deadline = finite ? NowNs() + duration : 0;
  return SelectUntil(nfds, readfds, writefds, exceptfds, finite, deadline);
}
