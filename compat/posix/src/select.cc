// gesher-base: select and pselect (<sys/select.h>) through poll, so they wait
// on every kind of descriptor poll takes (src/fd.cc). As on Linux, a
// descriptor that is not open fails the call with EBADF, the sets hold only
// the ready descriptors on return, and the result counts set bits.
#include <errno.h>
#include <poll.h>
#include <sys/select.h>
#include <windows.h>

#include <vector>

namespace {

int SelectMs(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, int timeout_ms) {
  if (nfds < 0 || nfds > FD_SETSIZE) return errno = EINVAL, -1;
  std::vector<pollfd> fds;
  for (int fd = 0; fd < nfds; ++fd) {
    short events = 0;
    if (readfds != nullptr && FD_ISSET(fd, readfds)) events |= POLLIN;
    if (writefds != nullptr && FD_ISSET(fd, writefds)) events |= POLLOUT;
    if (exceptfds != nullptr && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
    if (events != 0) fds.push_back({fd, events, 0});
  }
  if (fds.empty()) {
    // select(0, ...) is a sleep.
    Sleep(timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
    return 0;
  }
  int ready = poll(fds.data(), fds.size(), timeout_ms);
  if (ready < 0) return -1;
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

int ToMs(long long sec, long long nsec) {
  long long total = sec * 1000 + (nsec + 999999) / 1000000;  // round up
  return total > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(total);
}

}  // namespace

extern "C" int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                      struct timeval* timeout) {
  int ms = -1;
  if (timeout != nullptr) {
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0) return errno = EINVAL, -1;
    ms = ToMs(timeout->tv_sec, static_cast<long long>(timeout->tv_usec) * 1000);
  }
  int result = SelectMs(nfds, readfds, writefds, exceptfds, ms);
  // Linux leaves the time not slept in *timeout; callers that loop rely on it
  // reaching zero, which a completed wait gives.
  if (result == 0 && timeout != nullptr) timeout->tv_sec = timeout->tv_usec = 0;
  return result;
}

// Signals are not delivered asynchronously here (as for ppoll), so the mask
// has nothing to change while waiting.
extern "C" int pselect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                       const struct timespec* timeout, const sigset_t*) {
  int ms = -1;
  if (timeout != nullptr) {
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) {
      return errno = EINVAL, -1;
    }
    ms = ToMs(timeout->tv_sec, timeout->tv_nsec);
  }
  return SelectMs(nfds, readfds, writefds, exceptfds, ms);
}
