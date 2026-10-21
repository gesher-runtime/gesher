// gesher-base: file descriptors that may be sockets (see socket_internal.h)
// or /dev/zero and /dev/random (path.cc), and poll() over files, pipes and
// sockets.
//
// close, read, write, dup and dup2 are defined here under the CRT's own
// names: gesher-base is linked before the CRT, so every caller gets these.
// Each falls through to the CRT's underscore version for other fds.
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "internal.h"
#include "socket_internal.h"

namespace gesher {
namespace {

std::mutex table_mutex;
std::unordered_map<int, SocketFd> table;
// Lets the common case, a process without sockets, skip the lock.
std::atomic<size_t> socket_count{0};

// Takes the socket out of the table; false if fd is not a socket.
bool Remove(int fd, SocketFd* state) {
  if (socket_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(table_mutex);
  auto it = table.find(fd);
  if (it == table.end()) return false;
  *state = it->second;
  table.erase(it);
  socket_count.fetch_sub(1, std::memory_order_release);
  return true;
}

void Insert(int fd, const SocketFd& state) {
  std::lock_guard<std::mutex> lock(table_mutex);
  if (table.insert_or_assign(fd, state).second) {
    socket_count.fetch_add(1, std::memory_order_release);
  }
}

// /dev/zero and /dev/random fds (path.cc), also guarded by table_mutex.
std::unordered_map<int, Device> devices;
std::atomic<size_t> device_count{0};

bool LookupDevice(int fd, Device* device) {
  if (device_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(table_mutex);
  auto it = devices.find(fd);
  if (it == devices.end()) return false;
  *device = it->second;
  return true;
}

void ForgetDevice(int fd) {
  if (device_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(table_mutex);
  if (devices.erase(fd) != 0) device_count.fetch_sub(1, std::memory_order_release);
}

int ReadDevice(Device device, void* buf, unsigned int count) {
  if (device == Device::kZero) {
    memset(buf, 0, count);
  } else if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), count,
                                             BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return errno = EIO, -1;
  }
  return static_cast<int>(count);
}

// A CRT fd on NUL, to reserve a number for a socket.
int ReserveFd() {
  HANDLE h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return FailWithLastError();
  int fd = CrtResult(_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_NOINHERIT));
  if (fd < 0) CloseHandle(h);
  return fd;
}

}  // namespace

int RegisterSocket(const SocketFd& state) {
  int fd = ReserveFd();
  if (fd < 0) {
    int saved = errno;
    ws::Close(state.socket);
    errno = saved;
    return -1;
  }
  Insert(fd, state);
  return fd;
}

bool LookupSocket(int fd, SocketFd* state) {
  if (socket_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(table_mutex);
  auto it = table.find(fd);
  if (it == table.end()) return false;
  *state = it->second;
  return true;
}

void UpdateSocket(int fd, const SocketFd& state) {
  std::lock_guard<std::mutex> lock(table_mutex);
  auto it = table.find(fd);
  if (it != table.end()) it->second = state;
}

void RegisterDevice(int fd, Device device) {
  std::lock_guard<std::mutex> lock(table_mutex);
  if (devices.insert_or_assign(fd, device).second) {
    device_count.fetch_add(1, std::memory_order_release);
  }
}

}  // namespace gesher

using gesher::Device;
using gesher::SocketFd;

extern "C" {

int gesher_is_socket(int fd) {
  SocketFd state;
  return gesher::LookupSocket(fd, &state) ? 1 : 0;
}

int close(int fd) {
  SocketFd state;
  if (!gesher::Remove(fd, &state)) {
    gesher::ForgetDevice(fd);
    return gesher::CrtResult(_close(fd));
  }
  // Closing the socket first wakes threads blocked on it, as on Linux.
  int result = gesher::ws::Close(state.socket);
  int saved = errno;
  _close(fd);
  errno = saved;
  return result;
}

int read(int fd, void* buf, unsigned int count) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    Device device;
    if (gesher::LookupDevice(fd, &device)) return gesher::ReadDevice(device, buf, count);
    return gesher::CrtResult(_read(fd, buf, count));
  }
  gesher::ws::Buffer buffer = {buf, count};
  return static_cast<int>(gesher::ws::Recv(state.socket, &buffer, 1, 0, nullptr));
}

int write(int fd, const void* buf, unsigned int count) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) return gesher::CrtResult(_write(fd, buf, count));
  gesher::ws::Buffer buffer = {const_cast<void*>(buf), count};
  return static_cast<int>(gesher::ws::Send(state.socket, &buffer, 1, 0, nullptr));
}

int dup(int fd) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    int fd2 = gesher::CrtResult(_dup(fd));
    Device device;
    if (fd2 >= 0 && gesher::LookupDevice(fd, &device)) gesher::RegisterDevice(fd2, device);
    return fd2;
  }
  state.socket = gesher::ws::Duplicate(state.socket);
  if (state.socket == gesher::ws::kInvalidSocket) return -1;
  // Linux shares O_NONBLOCK between duplicates; here each starts as a copy.
  if (state.nonblocking) gesher::ws::SetNonBlocking(state.socket, true);
  return gesher::RegisterSocket(state);
}

int dup2(int fd, int fd2) {
  if (fd == fd2) return _get_osfhandle(fd) != -1 ? fd : (errno = EBADF, -1);
  SocketFd source;
  bool source_is_socket = gesher::LookupSocket(fd, &source);
  if (source_is_socket) {
    source.socket = gesher::ws::Duplicate(source.socket);
    if (source.socket == gesher::ws::kInvalidSocket) return -1;
    if (source.nonblocking) gesher::ws::SetNonBlocking(source.socket, true);
  }
  // Replacing a socket closes it, which wakes threads blocked on it; this is
  // how libcore's preClose0 interrupts them.
  SocketFd target;
  if (gesher::Remove(fd2, &target)) gesher::ws::Close(target.socket);
  if (gesher::CrtResult(_dup2(fd, fd2)) < 0) {
    if (source_is_socket) gesher::ws::Close(source.socket);
    return -1;
  }
  gesher::ForgetDevice(fd2);
  Device device;
  if (source_is_socket) {
    gesher::Insert(fd2, source);
  } else if (gesher::LookupDevice(fd, &device)) {
    gesher::RegisterDevice(fd2, device);
  }
  return fd2;
}

// The CRT calls any character device a terminal, NUL included, and so every
// socket fd (see above). Only a console is one, as in termios (misc.cc).
int isatty(int fd) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, 0;
  DWORD mode;
  if (gesher_is_socket(fd) || !GetConsoleMode(h, &mode)) return errno = ENOTTY, 0;
  return 1;
}

}  // extern "C"

// --- invalid fds ----------------------------------------------------------------

namespace {

// The CRT's default handler for invalid parameters ends the process, so
// close(-1) or a second close(fd) would crash instead of failing with EBADF
// as POSIX code expects. This one returns, and the CRT then reports the
// documented error.
void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}

const bool invalid_parameter_handler_installed =
    (_set_invalid_parameter_handler(IgnoreInvalidParameter), true);

}  // namespace

// --- poll -----------------------------------------------------------------------

namespace {

constexpr short kIn = POLLIN | POLLRDNORM;
constexpr short kOut = POLLOUT | POLLWRNORM;

// Readiness of a non-socket fd: pipes are readable when data is buffered or
// the writer is gone; files and other devices are always ready. Returns false
// for a pipe that has nothing yet.
bool PollFile(pollfd* p) {
  HANDLE h = gesher::HandleFromFd(p->fd);
  if (h == INVALID_HANDLE_VALUE) {
    p->revents = POLLNVAL;
    return true;
  }
  if (GetFileType(h) != FILE_TYPE_PIPE) {
    p->revents = p->events & (kIn | kOut);
    return true;
  }
  DWORD avail = 0;
  if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
    // The other end is closed: Linux reports POLLHUP, and POLLIN since read
    // returns 0. (PeekNamedPipe fails the same way on a pipe's write end.)
    p->revents = POLLHUP | (p->events & kIn) | (p->events & kOut);
    return true;
  }
  p->revents = (avail > 0 ? p->events & kIn : 0) | (p->events & kOut);
  return (p->events & kIn) == 0 || avail > 0;
}

}  // namespace

extern "C" int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  ULONGLONG deadline = timeout < 0 ? ~0ULL : GetTickCount64() + static_cast<ULONGLONG>(timeout);
  std::vector<gesher::ws::PollEntry> sockets;
  std::vector<nfds_t> socket_index;
  bool waiting_files = false;  // a pipe that may become readable
  int ready = 0;
  for (nfds_t i = 0; i < nfds; ++i) {
    pollfd* p = &fds[i];
    p->revents = 0;
    if (p->fd < 0) continue;  // ignored, as on Linux
    SocketFd state;
    if (gesher::LookupSocket(p->fd, &state)) {
      int events = ((p->events & kIn) != 0 ? gesher::ws::kPollIn : 0) |
                   ((p->events & kOut) != 0 ? gesher::ws::kPollOut : 0);
      sockets.push_back({state.socket, events, 0});
      socket_index.push_back(i);
    } else if (!PollFile(p)) {
      waiting_files = true;
    }
    if (p->revents != 0) ++ready;
  }

  for (;;) {
    int wait_ms;
    if (ready > 0) {
      wait_ms = 0;  // only collect what the sockets have
    } else if (waiting_files) {
      wait_ms = 10;  // pipes have no wait handle that WSAPoll accepts
    } else if (timeout < 0) {
      wait_ms = -1;
    } else {
      ULONGLONG now = GetTickCount64();
      wait_ms = now >= deadline ? 0 : static_cast<int>(deadline - now);
    }
    if (waiting_files && timeout >= 0) {
      ULONGLONG now = GetTickCount64();
      if (now >= deadline) wait_ms = 0;
      else if (deadline - now < static_cast<ULONGLONG>(wait_ms)) wait_ms = static_cast<int>(deadline - now);
    }

    if (!sockets.empty()) {
      if (gesher::ws::Poll(sockets.data(), sockets.size(), wait_ms) < 0) return -1;
      for (size_t k = 0; k < sockets.size(); ++k) {
        pollfd* p = &fds[socket_index[k]];
        int r = sockets[k].revents;
        short out = 0;
        if (r & gesher::ws::kPollIn) out |= p->events & kIn;
        if (r & gesher::ws::kPollOut) out |= p->events & kOut;
        if (r & gesher::ws::kPollErr) out |= POLLERR;
        if (r & gesher::ws::kPollHup) out |= POLLHUP;
        if (r & gesher::ws::kPollNval) out |= POLLNVAL;
        if (p->revents == 0 && out != 0) ++ready;
        p->revents = out;
      }
    } else if (ready == 0 && wait_ms != 0) {
      Sleep(wait_ms < 0 ? INFINITE : static_cast<DWORD>(wait_ms));
    }

    if (ready > 0 || timeout == 0 || (timeout > 0 && GetTickCount64() >= deadline)) return ready;
    if (waiting_files) {
      waiting_files = false;
      for (nfds_t i = 0; i < nfds; ++i) {
        pollfd* p = &fds[i];
        if (p->fd < 0 || p->revents != 0 || gesher_is_socket(p->fd)) continue;
        if (!PollFile(p)) waiting_files = true;
        if (p->revents != 0) ++ready;
      }
      if (ready > 0) return ready;
    }
  }
}

// Signals are not delivered asynchronously here, so the mask has nothing to
// change while waiting.
extern "C" int ppoll(struct pollfd* fds, nfds_t nfds, const struct timespec* timeout,
                     const void*) {
  int ms = -1;
  if (timeout != nullptr) {
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) {
      return errno = EINVAL, -1;
    }
    long long total = static_cast<long long>(timeout->tv_sec) * 1000 +
                      (timeout->tv_nsec + 999999) / 1000000;  // round up
    ms = total > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(total);
  }
  return poll(fds, nfds, ms);
}
