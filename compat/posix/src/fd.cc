// gesher-base: file descriptors that may be sockets (see socket_internal.h),
// /dev/zero and /dev/random (path.cc) or eventfds and epolls (epoll.cc), and
// poll() over files, pipes, sockets and eventfds.
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
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "binder_internal.h"
#include "event_internal.h"
#include "internal.h"
#include "record_lock_descriptors.h"
#include "socket_internal.h"

namespace gesher {
namespace {

std::mutex table_mutex;
std::unordered_map<int, SocketFd> table;
// Lets the common case, a process without sockets, skip the lock.
std::atomic<size_t> socket_count{0};

// Takes the socket out of the table; false if fd is not a socket.
bool Remove(int fd, SocketFd* state, bool* last) {
  if (socket_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(table_mutex);
  auto it = table.find(fd);
  if (it == table.end()) return false;
  *state = it->second;
  *last = --state->description->descriptor_refs == 0;
  table.erase(it);
  socket_count.fetch_sub(1, std::memory_order_release);
  return true;
}

void Insert(int fd, const SocketFd& state) {
  std::lock_guard<std::mutex> lock(table_mutex);
  if (table.insert_or_assign(fd, state).second) {
    ++state.description->descriptor_refs;
    socket_count.fetch_add(1, std::memory_order_release);
  }
}

// Publish a successful CRT replacement in one table transaction. The old
// native endpoint remains live until _dup2 has succeeded; a failed CRT call
// leaves its socket metadata and descriptor references untouched.
bool ReplaceSocket(int fd, const SocketFd* source, SocketFd* previous, bool* last) {
  std::lock_guard<std::mutex> lock(table_mutex);
  auto it = table.find(fd);
  const bool existed = it != table.end();
  if (existed) {
    *previous = it->second;
    *last = --previous->description->descriptor_refs == 0;
  }
  if (source) {
    if (existed) it->second = *source;
    else { table.emplace(fd, *source); socket_count.fetch_add(1, std::memory_order_release); }
    ++source->description->descriptor_refs;
  } else if (existed) {
    table.erase(it); socket_count.fetch_sub(1, std::memory_order_release);
  }
  return existed;
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

}  // namespace

int ReserveFd() {
  HANDLE h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return FailWithLastError();
  record_locks::DescriptorGuard descriptor_guard;
  int fd = CrtResult(_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_NOINHERIT));
  if (fd < 0) CloseHandle(h);
  return fd;
}

int RegisterSocket(const SocketFd& state) {
  record_locks::DescriptorGuard descriptor_guard;
  int fd = ReserveFd();
  if (fd < 0) {
    int saved = errno;
    // An alias registration failure must not close the shared native socket.
    if (!state.description) ws::Close(state.socket);
    errno = saved;
    return -1;
  }
  SocketFd registered = state;
  if (!registered.description) {
    registered.description = std::make_shared<ws::SocketDescription>();
    registered.description->provider = ws::ClassifyProvider(state.socket);
    registered.description->nonblocking.store(state.initial_nonblocking);
    const int datagram = ws::ConfigureOwnedDatagram(state.socket);
    if (datagram < 0) {
      const int saved = errno;
      _close(fd);
      ws::Close(state.socket);
      return errno = saved, -1;
    }
    registered.description->native_nonblocking_datagram = datagram != 0;
  }
  Insert(fd, registered);
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


void RegisterDevice(int fd, Device device) {
  std::lock_guard<std::mutex> lock(table_mutex);
  if (devices.insert_or_assign(fd, device).second) {
    device_count.fetch_add(1, std::memory_order_release);
  }
}

}  // namespace gesher

using gesher::Device;
using gesher::SocketFd;

namespace {
int CloseFdBody(int fd, bool unpublished, SocketFd* deferred = nullptr) {
  gesher::ForgetEpollEntries(fd);
  SocketFd state;
  int result;
  bool last = false;
  if (!gesher::Remove(fd, &state, &last)) {
    gesher::ForgetDevice(fd);
    gesher::ForgetEventObject(fd);
    gesher::ForgetBinder(fd);
    result = gesher::CrtResult(_close(fd));
  } else {
    // Only the last descriptor closes the native socket. Shared metadata
    // snapshots do not extend its lifetime or delay the existing close wake.
    if (last && deferred) {
      // Commit only CRT/table ownership under the descriptor transaction.
      // Physical close (including linger or a legacy call lease) runs after it.
      { std::lock_guard<std::mutex> lock(state.description->error_mutex); state.description->native_closing = true; }
      *deferred = state;
      result = 0;
    } else result = last ? gesher::ws::Close(state.socket, state.description) : 0;
    int socket_errno = errno;
    if (result < 0 && unpublished) {
      // A failed virtual-resource close does not make the private CRT slot
      // available for reuse. Keep both owners recorded for the caller's
      // explicit quarantine; do not discard the actual SOCKET handle.
      gesher::Insert(fd, state);
      return errno = socket_errno, -1;
    }
    int crt_closed = gesher::CrtResult(_close(fd));
    if (unpublished && crt_closed < 0) return -1;
    errno = socket_errno;
  }
  return result;
}

int CloseDescriptor(int fd) {
  SocketFd deferred;
  int result, saved, error;
  {
    gesher::record_locks::ClosingDescriptor transaction(fd);
    if (transaction.Error()) return errno = transaction.Error(), -1;
    result = CloseFdBody(fd, false, &deferred);
    saved = errno;
    error = transaction.Finish(result == 0);
  }
  if (deferred.description) {
    const int native_result = gesher::ws::Close(deferred.socket, deferred.description, true);
    if (native_result < 0) { result = native_result; saved = errno; }
  }
  if (error) return errno = error, -1;
  errno = saved;
  return result;
}
}  // namespace

namespace gesher {
int CloseUnpublishedDuplicate(int fd) {
  record_locks::DescriptorGuard descriptor_guard;
  return CloseFdBody(fd, true);
}
}  // namespace gesher

extern "C" {

int gesher_is_socket(int fd) {
  SocketFd state;
  return gesher::LookupSocket(fd, &state) ? 1 : 0;
}

int close(int fd) { return CloseDescriptor(fd); }

int read(int fd, void* buf, unsigned int count) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    Device device;
    if (gesher::LookupDevice(fd, &device)) return gesher::ReadDevice(device, buf, count);
    int result;
    if (gesher::ReadEventObject(fd, buf, count, &result)) return result;
    return gesher::CrtResult(_read(fd, buf, count));
  }
  gesher::ws::Buffer buffer = {buf, count};
  return static_cast<int>(gesher::ws::RecvOwned(state.socket, &buffer, 1, 0, nullptr, state.description));
}

int write(int fd, const void* buf, unsigned int count) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    int result;
    if (gesher::WriteEventObject(fd, buf, count, &result)) return result;
    result = gesher::CrtResult(_write(fd, buf, count));
    if (result > 0) gesher::InotifyNoteWrite(fd);
    return result;
  }
  gesher::ws::Buffer buffer = {const_cast<void*>(buf), count};
  return static_cast<int>(gesher::ws::SendOwned(state.socket, &buffer, 1, 0, nullptr, state.description));
}

int dup(int fd) {
  gesher::record_locks::DescriptorGuard descriptor_guard;
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    int fd2 = gesher::CrtResult(_dup(fd));
    Device device;
    if (fd2 >= 0 && gesher::LookupDevice(fd, &device)) gesher::RegisterDevice(fd2, device);
    if (fd2 >= 0) gesher::DupEventObject(fd, fd2);
    if (fd2 >= 0) gesher::DupBinder(fd, fd2);
    return fd2;
  }
  // WSADuplicateSocket creates distinct native connect-error ownership.
  // POSIX aliases instead share the same native socket and last-close count.
  return gesher::RegisterSocket(state);
}

int dup2(int fd, int fd2) {
  SocketFd target;
  bool close_target = false;
  int saved, metadata_error;
  {
    gesher::record_locks::DescriptorGuard descriptor_guard;
    if (fd == fd2) return _get_osfhandle(fd) != -1 ? fd : (errno = EBADF, -1);
    if (gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
    gesher::record_locks::ClosingDescriptor transaction(fd2);
    if (transaction.Error()) return errno = transaction.Error(), -1;
    SocketFd source;
    bool source_is_socket = gesher::LookupSocket(fd, &source);
    if (gesher::CrtResult(_dup2(fd, fd2)) < 0) {
      int saved = errno;
      int error = transaction.Finish();
      errno = error ? error : saved;
      return -1;
    }
    saved = errno;
    bool last = false;
    close_target = gesher::ReplaceSocket(fd2, source_is_socket ? &source : nullptr, &target, &last) && last;
    if (close_target) { std::lock_guard<std::mutex> lock(target.description->error_mutex); target.description->native_closing = true; }
    metadata_error = transaction.Finish(true);
    // fd2's old file is closed: forget what it was.
    gesher::ForgetDevice(fd2);
    gesher::ForgetEventObject(fd2);
    gesher::ForgetEpollEntries(fd2);
    gesher::ForgetBinder(fd2);
    Device device;
    if (!source_is_socket && gesher::LookupDevice(fd, &device)) {
      gesher::RegisterDevice(fd2, device);
    } else if (!source_is_socket) {
      gesher::DupEventObject(fd, fd2);
      gesher::DupBinder(fd, fd2);
    }
  }
  // POSIX dup2 does not report the replaced endpoint's close error. A failed
  // native close still retains explicit ownership before success is published.
  if (close_target) gesher::ws::Close(target.socket, target.description, true);
  if (metadata_error) return errno = metadata_error, -1;
  errno = saved;
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

// Linux has no text mode: files the CRT opens without an explicit mode
// (fopen(path, "r"), _wopen without _O_TEXT) are binary, so reads return
// the file's bytes, CRs and Ctrl-Z included.
const bool binary_file_mode_set = (_set_fmode(_O_BINARY), true);

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

namespace gesher {

static int PollFdsImpl(pollfd* fds, nfds_t nfds, int timeout, HANDLE interrupt, bool* interrupted, PollObserver observer, void* context,
                       ws::NativePollObserver native_observer, void* native_context) {
  ULONGLONG deadline = gesher::DeadlineAfter(timeout);
  std::vector<ws::PollEntry> sockets;
  std::vector<nfds_t> socket_index;
  // Held so that their events stay open while waited on.
  std::vector<std::shared_ptr<EventFd>> eventfds;
  std::vector<nfds_t> eventfd_index;
  bool waiting_files = false;  // a pipe that may become readable
  for (nfds_t i = 0; i < nfds; ++i) {
    pollfd* p = &fds[i];
    p->revents = 0;
    if (p->fd < 0) continue;  // ignored, as on Linux
    SocketFd state;
    if (std::shared_ptr<EventFd> e = LookupEventFd(p->fd)) {
      p->revents = PollEventFd(*e, p->events, nullptr);
      eventfds.push_back(std::move(e));
      eventfd_index.push_back(i);
    } else if (LookupSocket(p->fd, &state)) {
      int events = ((p->events & kIn) != 0 ? ws::kPollIn : 0) |
                   ((p->events & kOut) != 0 ? ws::kPollOut : 0) |
                   ((p->events & POLLPRI) != 0 ? ws::kPollPri : 0);
      sockets.push_back({state.socket, events, 0, state.description});
      socket_index.push_back(i);
    } else if (!PollFile(p)) {
      waiting_files = true;
    }
  }
  if (observer) observer(context);  // real snapshots held; no descriptor/table lock
  auto count_ready = [&] {
    int n = 0;
    for (nfds_t i = 0; i < nfds; ++i) n += fds[i].fd >= 0 && fds[i].revents != 0;
    return n;
  };
  int ready = count_ready();
  bool unused;
  if (interrupted == nullptr) interrupted = &unused;
  *interrupted = false;

  // Eventfds and `interrupt` are Win32 events, waited for together. Pipes have
  // no wait handle; sockets need bounded identity revalidation. All slices
  // retain the one caller deadline rather than restarting its timeout.
  // Socket numbers may be closed/reused while waiting. Bound every native
  // sample, then revalidate the exact description before delivering readiness.
  bool sliced = waiting_files || !sockets.empty();
  std::vector<HANDLE> handles;
  for (;;) {
    int wait_ms = 0;  // with something ready, only collect what the rest has
    if (ready == 0) {
      if (timeout < 0) {
        wait_ms = -1;
      } else {
        wait_ms = gesher::MillisUntil(deadline);
      }
      if (sliced && (wait_ms < 0 || wait_ms > 10)) wait_ms = 10;
    }

    if (!sockets.empty()) {
      const int sampled = ws::Poll(sockets.data(), sockets.size(), wait_ms, native_observer, native_context);
      native_observer = nullptr;  // one real native sample, never a mutable global hook
      if (sampled < 0) return -1;
      for (size_t k = 0; k < sockets.size(); ++k) {
        pollfd* p = &fds[socket_index[k]];
        SocketFd current;
        if (!LookupSocket(p->fd, &current) || current.description != sockets[k].description) {
          p->revents = POLLNVAL;
          continue;
        }
        int r = sockets[k].revents;
        short out = 0;
        if (r & ws::kPollIn) out |= p->events & kIn;
        if (r & ws::kPollOut) out |= p->events & kOut;
        if (r & ws::kPollPri) out |= p->events & POLLPRI;
        if (r & ws::kPollErr) out |= POLLERR;
        if (r & ws::kPollHup) out |= POLLHUP;
        if (r & ws::kPollNval) out |= POLLNVAL;
        p->revents = out;
      }
    } else if (wait_ms != 0) {
      handles.clear();
      for (size_t k = 0; k < eventfds.size(); ++k) {
        pollfd* p = &fds[eventfd_index[k]];
        HANDLE wake = nullptr;
        p->revents = PollEventFd(*eventfds[k], p->events, &wake);
        if (p->revents != 0) wait_ms = 0;  // ready since the last look
        if (wake != nullptr) handles.push_back(wake);
      }
      if (interrupt != nullptr) handles.push_back(interrupt);
      if (handles.size() > MAXIMUM_WAIT_OBJECTS) {
        handles.clear();  // too many to wait for: look every 10 ms
        if (wait_ms < 0 || wait_ms > 10) wait_ms = 10;
      }
      if (wait_ms != 0 && handles.empty()) {
        Sleep(wait_ms < 0 ? INFINITE : static_cast<DWORD>(wait_ms));
      } else if (wait_ms != 0) {
        DWORD n = static_cast<DWORD>(handles.size());
        DWORD w = WaitForMultipleObjects(n, handles.data(), FALSE,
                                         wait_ms < 0 ? INFINITE : static_cast<DWORD>(wait_ms));
        if (w == WAIT_FAILED) return FailWithLastError();
        // The auto-reset `interrupt` is consumed by the wait that sees it.
        if (interrupt != nullptr && w == WAIT_OBJECT_0 + n - 1) *interrupted = true;
      }
    }

    for (size_t k = 0; k < eventfds.size(); ++k) {
      pollfd* p = &fds[eventfd_index[k]];
      p->revents = PollEventFd(*eventfds[k], p->events, nullptr);
    }
    if (interrupt != nullptr && !*interrupted &&
        WaitForSingleObject(interrupt, 0) == WAIT_OBJECT_0) {
      *interrupted = true;
    }
    if (waiting_files) {
      waiting_files = false;
      for (nfds_t i = 0; i < nfds; ++i) {
        pollfd* p = &fds[i];
        SocketFd state;
        if (p->fd < 0 || p->revents != 0 || LookupSocket(p->fd, &state) || LookupEventFd(p->fd)) {
          continue;
        }
        if (!PollFile(p)) waiting_files = true;
      }
    }
    ready = count_ready();
    if (ready > 0 || *interrupted || timeout == 0 ||
        (timeout > 0 && gesher::MonotonicMicros() >= deadline)) {
      return ready;
    }
  }
}

int PollFds(pollfd* fds, nfds_t nfds, int timeout, HANDLE interrupt, bool* interrupted) {
  return PollFdsImpl(fds, nfds, timeout, interrupt, interrupted, nullptr, nullptr, nullptr, nullptr);
}

int PollWithObserver(void* records, size_t count, int timeout_ms, PollObserver observer, void* context,
                     ws::NativePollObserver native_observer, void* native_context) {
  if (static_cast<size_t>(static_cast<nfds_t>(count)) != count) return errno = EINVAL, -1;
  return PollFdsImpl(static_cast<pollfd*>(records), static_cast<nfds_t>(count), timeout_ms, nullptr, nullptr, observer, context, native_observer, native_context);
}

}  // namespace gesher

extern "C" int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  return gesher::PollFds(fds, nfds, timeout, nullptr, nullptr);
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
