#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "pipe_internal.h"

#include <errno.h>
#include <io.h>
#ifndef POLLIN
#include <poll.h>
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "internal.h"

#ifndef SIGPIPE
#define SIGPIPE 13
#endif

extern "C" int kill(int pid, int sig);

namespace gesher {
namespace {

std::mutex pipe_table_mutex;
std::unordered_map<int, PipeFd> pipe_table;
std::atomic<size_t> pipe_count{0};

}  // namespace

void RegisterPipe(int read_fd, int write_fd, bool nonblocking) {
  HANDLE r_handle = HandleFromFd(read_fd);
  HANDLE w_handle = HandleFromFd(write_fd);
  HANDLE r_dup = INVALID_HANDLE_VALUE;
  HANDLE w_dup = INVALID_HANDLE_VALUE;
  if (r_handle != INVALID_HANDLE_VALUE) {
    DuplicateHandle(GetCurrentProcess(), r_handle, GetCurrentProcess(), &r_dup, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
  }
  if (w_handle != INVALID_HANDLE_VALUE) {
    DuplicateHandle(GetCurrentProcess(), w_handle, GetCurrentProcess(), &w_dup, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
  }

  auto state = std::make_shared<PipeState>();
  state->read_handle = r_dup;
  state->write_handle = w_dup;
  state->capacity = 64 * 1024;
  state->readers = 1;
  state->writers = 1;

  auto read_endpoint = std::make_shared<PipeEndpoint>();
  read_endpoint->direction = PipeDirection::kRead;
  read_endpoint->nonblocking.store(nonblocking, std::memory_order_relaxed);
  read_endpoint->state = state;

  auto write_endpoint = std::make_shared<PipeEndpoint>();
  write_endpoint->direction = PipeDirection::kWrite;
  write_endpoint->nonblocking.store(nonblocking, std::memory_order_relaxed);
  write_endpoint->state = state;

  {
    std::lock_guard<std::mutex> lock(pipe_table_mutex);
    pipe_table[read_fd] = PipeFd{read_endpoint};
    pipe_table[write_fd] = PipeFd{write_endpoint};
    pipe_count.fetch_add(2, std::memory_order_release);
  }
}

bool LookupPipe(int fd, PipeFd* out) {
  if (pipe_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(pipe_table_mutex);
  auto it = pipe_table.find(fd);
  if (it == pipe_table.end()) return false;
  if (out != nullptr) *out = it->second;
  return true;
}

void ForgetPipe(int fd) {
  std::shared_ptr<PipeEndpoint> endpoint;
  {
    if (pipe_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard<std::mutex> lock(pipe_table_mutex);
    auto it = pipe_table.find(fd);
    if (it == pipe_table.end()) return;
    endpoint = std::move(it->second.endpoint);
    pipe_table.erase(it);
    pipe_count.fetch_sub(1, std::memory_order_release);
  }
  if (!endpoint || !endpoint->state) return;
  auto state = endpoint->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (endpoint->direction == PipeDirection::kRead) {
    if (--state->readers == 0) {
      if (state->read_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(state->read_handle);
        state->read_handle = INVALID_HANDLE_VALUE;
      }
      state->writable_cv.notify_all();
      WakePollers();
    }
  } else {
    if (--state->writers == 0) {
      if (state->write_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(state->write_handle);
        state->write_handle = INVALID_HANDLE_VALUE;
      }
      state->readable_cv.notify_all();
      WakePollers();
    }
  }
}

void DupPipe(int old_fd, int new_fd) {
  std::shared_ptr<PipeEndpoint> endpoint;
  {
    if (pipe_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard<std::mutex> lock(pipe_table_mutex);
    auto it = pipe_table.find(old_fd);
    if (it == pipe_table.end()) return;
    endpoint = it->second.endpoint;
    pipe_table[new_fd] = PipeFd{endpoint};
    pipe_count.fetch_add(1, std::memory_order_release);
  }
  if (endpoint && endpoint->state) {
    std::lock_guard<std::mutex> lock(endpoint->state->mutex);
    if (endpoint->direction == PipeDirection::kRead) {
      ++endpoint->state->readers;
    } else {
      ++endpoint->state->writers;
    }
  }
}

bool ReadPipe(int fd, void* buf, unsigned int count, int* result) {
  PipeFd pipe_fd;
  if (!LookupPipe(fd, &pipe_fd)) return false;

  auto endpoint = pipe_fd.endpoint;
  if (endpoint->direction != PipeDirection::kRead) {
    errno = EBADF;
    *result = -1;
    return true;
  }

  if (count == 0) {
    *result = 0;
    return true;
  }

  auto state = endpoint->state;
  std::unique_lock<std::mutex> lock(state->mutex);

  for (;;) {
    DWORD avail = 0;
    BOOL ok = FALSE;
    HANDLE h = (state->read_handle != INVALID_HANDLE_VALUE) ? state->read_handle : HandleFromFd(fd);
    if (h != INVALID_HANDLE_VALUE) {
      ok = PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr);
    }

    if (avail > 0) {
      size_t to_read = (std::min)(static_cast<size_t>(count), static_cast<size_t>(avail));
      int n = _read(fd, buf, static_cast<unsigned int>(to_read));
      if (n > 0) {
        state->writable_cv.notify_all();
        WakePollers();
      }
      *result = (n < 0) ? gesher::CrtResult(n) : n;
      return true;
    }

    bool writers_closed = (state->writers == 0) || (!ok && GetLastError() == ERROR_BROKEN_PIPE);
    if (writers_closed) {
      *result = 0;
      return true;
    }

    if (endpoint->nonblocking.load(std::memory_order_acquire)) {
      errno = EAGAIN;
      *result = -1;
      return true;
    }

    state->readable_cv.wait(lock);
  }
}

bool WritePipe(int fd, const void* buf, unsigned int count, int* result) {
  PipeFd pipe_fd;
  if (!LookupPipe(fd, &pipe_fd)) return false;

  auto endpoint = pipe_fd.endpoint;
  if (endpoint->direction != PipeDirection::kWrite) {
    errno = EBADF;
    *result = -1;
    return true;
  }

  if (count == 0) {
    *result = 0;
    return true;
  }

  auto state = endpoint->state;
  std::unique_lock<std::mutex> lock(state->mutex);

  if (state->readers == 0) {
    kill(static_cast<int>(GetCurrentProcessId()), SIGPIPE);
    errno = EPIPE;
    *result = -1;
    return true;
  }

  size_t total_written = 0;
  while (total_written < count) {
    if (state->readers == 0) {
      if (total_written > 0) {
        *result = static_cast<int>(total_written);
        return true;
      }
      kill(static_cast<int>(GetCurrentProcessId()), SIGPIPE);
      errno = EPIPE;
      *result = -1;
      return true;
    }

    DWORD avail = 0;
    BOOL ok = FALSE;
    if (state->read_handle != INVALID_HANDLE_VALUE) {
      ok = PeekNamedPipe(state->read_handle, nullptr, 0, nullptr, &avail, nullptr);
    }
    if (!ok && state->read_handle != INVALID_HANDLE_VALUE) {
      if (total_written > 0) {
        *result = static_cast<int>(total_written);
        return true;
      }
      kill(static_cast<int>(GetCurrentProcessId()), SIGPIPE);
      errno = EPIPE;
      *result = -1;
      return true;
    }

    size_t space = (avail < state->capacity) ? (state->capacity - avail) : 0;
    if (space == 0) {
      if (total_written > 0) {
        *result = static_cast<int>(total_written);
        return true;
      }
      if (endpoint->nonblocking.load(std::memory_order_acquire)) {
        errno = EAGAIN;
        *result = -1;
        return true;
      }
      state->writable_cv.wait(lock);
      continue;
    }

    size_t remaining = count - total_written;
    size_t to_write = (std::min)(remaining, space);
    int n = _write(fd, static_cast<const char*>(buf) + total_written, static_cast<unsigned int>(to_write));
    if (n < 0) {
      if (total_written > 0) {
        *result = static_cast<int>(total_written);
        return true;
      }
      if (errno == EINVAL && (_doserrno == ERROR_NO_DATA || _doserrno == ERROR_BROKEN_PIPE)) {
        kill(static_cast<int>(GetCurrentProcessId()), SIGPIPE);
        errno = EPIPE;
        *result = -1;
        return true;
      }
      *result = gesher::CrtResult(n);
      return true;
    }

    total_written += n;
    state->readable_cv.notify_all();
    WakePollers();

    if (endpoint->nonblocking.load(std::memory_order_acquire)) {
      break;
    }
  }

  *result = static_cast<int>(total_written);
  return true;
}

bool PollPipe(int fd, short events, short* revents) {
  PipeFd pipe_fd;
  if (!LookupPipe(fd, &pipe_fd)) return false;

  auto endpoint = pipe_fd.endpoint;
  auto state = endpoint->state;
  std::lock_guard<std::mutex> lock(state->mutex);

  *revents = 0;

  if (endpoint->direction == PipeDirection::kRead) {
    DWORD avail = 0;
    BOOL ok = FALSE;
    HANDLE h = (state->read_handle != INVALID_HANDLE_VALUE) ? state->read_handle : HandleFromFd(fd);
    if (h != INVALID_HANDLE_VALUE) {
      ok = PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr);
    }
    bool writers_closed = (state->writers == 0) || (!ok && GetLastError() == ERROR_BROKEN_PIPE);
    if (writers_closed) {
      *revents |= POLLHUP;
      if (events & (POLLIN | POLLRDNORM)) *revents |= (events & (POLLIN | POLLRDNORM));
    } else if (avail > 0) {
      if (events & (POLLIN | POLLRDNORM)) *revents |= (events & (POLLIN | POLLRDNORM));
    }
  } else {
    // Write end
    if (state->readers == 0) {
      *revents |= POLLERR;
    } else {
      DWORD avail = 0;
      BOOL ok = FALSE;
      if (state->read_handle != INVALID_HANDLE_VALUE) {
        ok = PeekNamedPipe(state->read_handle, nullptr, 0, nullptr, &avail, nullptr);
      }
      if (!ok && state->read_handle != INVALID_HANDLE_VALUE) {
        *revents |= POLLERR;
      } else {
        size_t space = (avail < state->capacity) ? (state->capacity - avail) : 0;
        if (space > 0) {
          if (events & (POLLOUT | POLLWRNORM)) *revents |= (events & (POLLOUT | POLLWRNORM));
        }
      }
    }
  }

  return true;
}

}  // namespace gesher
