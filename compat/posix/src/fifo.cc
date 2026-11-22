// gesher-base: POSIX FIFOs (mkfifo, S_IFIFO open/read/write/poll/fstat/fcntl).
//
// mkfifo() writes a small 16-byte magic marker file on disk so paths,
// unlink(), rename(), EEXIST/ENOENT, and NTFS file identity work like
// ordinary filesystem entries. Opening that marker switches to an in-process
// ring buffer keyed by the file's volume serial and NTFS file index, with
// POSIX reader/writer rendezvous, EOF, EPIPE, ENXIO, FIONREAD, and poll/epoll
// wake integration.
#include "fifo_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "event_internal.h"
#include "internal.h"
#include "record_lock_descriptors.h"

namespace gesher {
namespace {

constexpr char kFifoMagic[16] = {
    '\0', 'G', 'E', 'S', 'H', 'E', 'R', '_', 'F', 'I', 'F', 'O', '_', 'V', '1', '\n',
};
constexpr size_t kFifoBufferSize = 64 * 1024;

struct FifoKey {
  DWORD volume = 0;
  DWORD index_high = 0;
  DWORD index_low = 0;

  bool operator<(const FifoKey& o) const {
    if (volume != o.volume) return volume < o.volume;
    if (index_high != o.index_high) return index_high < o.index_high;
    return index_low < o.index_low;
  }
};

struct FifoState {
  FifoKey key;
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> buf;
  size_t head = 0;
  size_t size = 0;
  int readers = 0;
  int writers = 0;
  bool had_writer = false;

  explicit FifoState(const FifoKey& k) : key(k), buf(kFifoBufferSize) {}
  size_t FreeSpace() const { return buf.size() - size; }
};

std::mutex g_fifo_registry_mutex;
std::map<FifoKey, std::weak_ptr<FifoState>> g_fifo_registry;

struct FifoEndpoint {
  std::shared_ptr<FifoState> state;
  FifoKey key;
  unsigned long long dev = 0;
  unsigned long long ino = 0;
  int access = 0;
  bool can_read = false;
  bool can_write = false;
  std::atomic<bool> nonblocking{false};

  ~FifoEndpoint() {
    if (!state) return;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      if (can_read) state->readers--;
      if (can_write) state->writers--;
    }
    state->cv.notify_all();
    state.reset();
    {
      std::lock_guard<std::mutex> reg_lock(g_fifo_registry_mutex);
      auto it = g_fifo_registry.find(key);
      if (it != g_fifo_registry.end() && it->second.expired()) {
        g_fifo_registry.erase(it);
      }
    }
    if (HasPollWaiters()) WakePollers();
  }
};

std::mutex g_fifo_fds_mutex;
std::unordered_map<int, std::shared_ptr<FifoEndpoint>> g_fifo_fds;

std::shared_ptr<FifoEndpoint> LookupFifoEndpoint(int fd) {
  std::lock_guard<std::mutex> lock(g_fifo_fds_mutex);
  auto it = g_fifo_fds.find(fd);
  return it == g_fifo_fds.end() ? nullptr : it->second;
}

std::shared_ptr<FifoState> GetOrCreateFifoState(const FifoKey& key) {
  std::lock_guard<std::mutex> lock(g_fifo_registry_mutex);
  auto& weak = g_fifo_registry[key];
  std::shared_ptr<FifoState> state = weak.lock();
  if (!state) {
    state = std::make_shared<FifoState>(key);
    weak = state;
  }
  return state;
}

bool HasMagicHeader(const std::wstring& windows) {
  HANDLE probe = CreateFileW(windows.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (probe == INVALID_HANDLE_VALUE) return false;
  char header[sizeof(kFifoMagic)];
  DWORD n = 0;
  bool match = ReadFile(probe, header, sizeof(header), &n, nullptr) &&
               n == sizeof(kFifoMagic) &&
               std::memcmp(header, kFifoMagic, sizeof(kFifoMagic)) == 0;
  CloseHandle(probe);
  return match;
}

}  // namespace

bool IsFifoPath(const std::wstring& windows) {
  HANDLE h = CreateFileW(windows.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info;
  bool match = false;
  if (GetFileInformationByHandle(h, &info) &&
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
      info.nFileSizeHigh == 0 && info.nFileSizeLow == sizeof(kFifoMagic)) {
    char header[sizeof(kFifoMagic)];
    DWORD n = 0;
    if (ReadFile(h, header, sizeof(header), &n, nullptr) &&
        n == sizeof(kFifoMagic) &&
        std::memcmp(header, kFifoMagic, sizeof(kFifoMagic)) == 0) {
      match = true;
    }
  }
  CloseHandle(h);
  return match;
}

bool TryOpenFifo(const std::wstring& windows, HANDLE existing_handle, int flags, int* out_fd) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(existing_handle, &info)) return false;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
  if (info.nFileSizeHigh != 0 || info.nFileSizeLow != sizeof(kFifoMagic)) return false;
  if (!HasMagicHeader(windows)) return false;

  // Release the filesystem handle so unlink() succeeds cleanly while open.
  CloseHandle(existing_handle);

  const FifoKey key{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
  std::shared_ptr<FifoState> state = GetOrCreateFifoState(key);

  const int access = flags & O_ACCMODE;
  const bool can_read = (access == O_RDONLY || access == O_RDWR);
  const bool can_write = (access == O_WRONLY || access == O_RDWR);
  const bool nonblock = (flags & O_NONBLOCK) != 0;

  {
    std::unique_lock<std::mutex> lock(state->mu);
    if (can_read) state->readers++;
    if (can_write) {
      state->writers++;
      state->had_writer = true;
    }
    state->cv.notify_all();

    if (access == O_WRONLY && nonblock && state->readers == 0) {
      state->writers--;
      state->cv.notify_all();
      *out_fd = -1;
      errno = ENXIO;
      return true;
    }

    if (!nonblock) {
      if (access == O_RDONLY) {
        while (state->writers == 0) state->cv.wait(lock);
      } else if (access == O_WRONLY) {
        while (state->readers == 0) state->cv.wait(lock);
      }
    }
  }

  auto endpoint = std::make_shared<FifoEndpoint>();
  endpoint->state = state;
  endpoint->key = key;
  endpoint->dev = info.dwVolumeSerialNumber;
  endpoint->ino = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  endpoint->access = access;
  endpoint->can_read = can_read;
  endpoint->can_write = can_write;
  endpoint->nonblocking.store(nonblock, std::memory_order_relaxed);

  int fd = -1;
  {
    record_locks::DescriptorGuard descriptor_guard;
    fd = ReserveFd();
    if (fd >= 0) {
      if ((flags & O_CLOEXEC) == 0) {
        HANDLE nul = HandleFromFd(fd);
        if (nul != INVALID_HANDLE_VALUE) {
          SetHandleInformation(nul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
      }
      std::lock_guard<std::mutex> lock(g_fifo_fds_mutex);
      g_fifo_fds.emplace(fd, endpoint);
    }
  }
  if (fd < 0) {
    const int saved = errno;
    endpoint.reset();
    errno = saved;
    *out_fd = -1;
    return true;
  }

  if (HasPollWaiters()) WakePollers();
  *out_fd = fd;
  return true;
}

bool IsFifoFd(int fd) { return LookupFifoEndpoint(fd) != nullptr; }

bool ReadFifo(int fd, void* buf, unsigned int count, int* result) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return false;
  if (!ep->can_read) {
    *result = -1;
    errno = EBADF;
    return true;
  }
  if (count == 0) {
    *result = 0;
    return true;
  }
  if (buf == nullptr) {
    *result = -1;
    errno = EFAULT;
    return true;
  }

  FifoState& st = *ep->state;
  size_t copied = 0;
  {
    std::unique_lock<std::mutex> lock(st.mu);
    while (st.size == 0) {
      if (st.writers == 0) {
        *result = 0;
        return true;
      }
      if (ep->nonblocking.load(std::memory_order_acquire)) {
        *result = -1;
        errno = EAGAIN;
        return true;
      }
      st.cv.wait(lock);
    }
    const size_t to_read = std::min<size_t>(count, st.size);
    const size_t first = std::min(to_read, st.buf.size() - st.head);
    std::memcpy(buf, st.buf.data() + st.head, first);
    if (to_read > first) {
      std::memcpy(static_cast<uint8_t*>(buf) + first, st.buf.data(), to_read - first);
    }
    st.head = (st.head + to_read) % st.buf.size();
    st.size -= to_read;
    copied = to_read;
    st.cv.notify_all();
  }
  if (HasPollWaiters()) WakePollers();
  *result = static_cast<int>(copied);
  return true;
}

bool WriteFifo(int fd, const void* buf, unsigned int count, int* result) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return false;
  if (!ep->can_write) {
    *result = -1;
    errno = EBADF;
    return true;
  }
  if (count == 0) {
    *result = 0;
    return true;
  }
  if (buf == nullptr) {
    *result = -1;
    errno = EFAULT;
    return true;
  }

  FifoState& st = *ep->state;
  const size_t total = std::min<size_t>(count, static_cast<size_t>(INT_MAX));
  size_t written = 0;

  while (written < total) {
    bool wrote_chunk = false;
    {
      std::unique_lock<std::mutex> lock(st.mu);
      if (st.readers == 0) {
        if (written > 0) break;
        *result = -1;
        errno = EPIPE;
        return true;
      }
      while (st.FreeSpace() == 0) {
        if (ep->nonblocking.load(std::memory_order_acquire)) {
          if (written > 0) {
            *result = static_cast<int>(written);
            return true;
          }
          *result = -1;
          errno = EAGAIN;
          return true;
        }
        st.cv.wait(lock);
        if (st.readers == 0) {
          if (written > 0) {
            *result = static_cast<int>(written);
            return true;
          }
          *result = -1;
          errno = EPIPE;
          return true;
        }
      }
      const size_t chunk = std::min(total - written, st.FreeSpace());
      const size_t tail = (st.head + st.size) % st.buf.size();
      const size_t first = std::min(chunk, st.buf.size() - tail);
      std::memcpy(st.buf.data() + tail, static_cast<const uint8_t*>(buf) + written, first);
      if (chunk > first) {
        std::memcpy(st.buf.data(), static_cast<const uint8_t*>(buf) + written + first, chunk - first);
      }
      st.size += chunk;
      written += chunk;
      wrote_chunk = chunk > 0;
      st.cv.notify_all();
    }
    if (wrote_chunk && HasPollWaiters()) WakePollers();
    if (ep->nonblocking.load(std::memory_order_acquire)) break;
  }

  *result = static_cast<int>(written);
  return true;
}

int FifoWriteError(int fd) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return 0;
  return ep->can_write ? 0 : EBADF;
}

bool PollFifo(int fd, short events, short* revents) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return false;

  constexpr short kIn = POLLIN | POLLRDNORM;
  constexpr short kOut = POLLOUT | POLLWRNORM;

  bool has_data = false;
  bool has_space = false;
  int readers = 0;
  int writers = 0;
  bool had_writer = false;
  {
    std::lock_guard<std::mutex> lock(ep->state->mu);
    has_data = ep->state->size > 0;
    has_space = ep->state->FreeSpace() > 0;
    readers = ep->state->readers;
    writers = ep->state->writers;
    had_writer = ep->state->had_writer;
  }

  short out = 0;
  if (ep->can_read) {
    if (has_data) out |= static_cast<short>(events & kIn);
    if (had_writer && writers == 0) {
      out |= POLLHUP | static_cast<short>(events & kIn);
    }
  }
  if (ep->can_write) {
    if (readers == 0) {
      out |= POLLERR;
    } else if (has_space) {
      out |= static_cast<short>(events & kOut);
    }
  }
  *revents = out;
  return true;
}

bool FstatFifo(int fd, struct stat* buf) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return false;
  std::memset(buf, 0, sizeof(*buf));
  buf->st_dev = ep->dev;
  buf->st_ino = ep->ino;
  buf->st_nlink = 1;
  buf->st_mode = S_IFIFO | 0600;
  buf->st_uid = getuid();
  buf->st_gid = getgid();
  buf->st_blksize = 4096;
  return true;
}

bool FifoAvailable(int fd, int* bytes) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return false;
  std::lock_guard<std::mutex> lock(ep->state->mu);
  const size_t n = ep->state->size;
  *bytes = static_cast<int>(n > static_cast<size_t>(INT_MAX) ? static_cast<size_t>(INT_MAX) : n);
  return true;
}

bool FcntlFifo(int fd, int cmd, long arg, int* result) {
  std::shared_ptr<FifoEndpoint> ep = LookupFifoEndpoint(fd);
  if (!ep) return false;
  if (cmd == F_GETFL) {
    *result = ep->access | (ep->nonblocking.load(std::memory_order_acquire) ? O_NONBLOCK : 0);
    return true;
  }
  if (cmd == F_SETFL) {
    ep->nonblocking.store((arg & O_NONBLOCK) != 0, std::memory_order_release);
    *result = 0;
    return true;
  }
  return false;
}

void DupFifo(int fd, int fd2) {
  std::shared_ptr<FifoEndpoint> replaced;
  {
    std::lock_guard<std::mutex> lock(g_fifo_fds_mutex);
    auto it = g_fifo_fds.find(fd);
    if (it == g_fifo_fds.end()) return;
    auto dst = g_fifo_fds.find(fd2);
    if (dst != g_fifo_fds.end()) {
      if (dst->second == it->second) return;
      replaced = std::move(dst->second);
      dst->second = it->second;
    } else {
      g_fifo_fds.emplace(fd2, it->second);
    }
  }
}

bool ForgetFifo(int fd) {
  std::shared_ptr<FifoEndpoint> dropped;
  {
    std::lock_guard<std::mutex> lock(g_fifo_fds_mutex);
    auto it = g_fifo_fds.find(fd);
    if (it == g_fifo_fds.end()) return false;
    dropped = std::move(it->second);
    g_fifo_fds.erase(it);
  }
  return true;
}

}  // namespace gesher

extern "C" int mkfifo(const char* path, mode_t mode) {
  (void)mode;
  if (path == nullptr) return errno = EFAULT, -1;
  if (*path == '\0') return errno = ENOENT, -1;
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EEXIST)) return -1;
  HANDLE h = CreateFileW(windows.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  DWORD written = 0;
  BOOL ok = WriteFile(h, gesher::kFifoMagic, sizeof(gesher::kFifoMagic), &written, nullptr);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(h);
  if (!ok || written != sizeof(gesher::kFifoMagic)) {
    DeleteFileW(windows.c_str());
    SetLastError(ok ? ERROR_WRITE_FAULT : error);
    return gesher::FailWithLastError();
  }
  return 0;
}
