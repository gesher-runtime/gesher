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
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <windows.h>
#include <bcrypt.h>
#include <gesher/file_description.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <vector>

#include "binder_internal.h"
#include "descriptor_resources.h"
#include "event_internal.h"
#include "internal.h"
#include "revocable_internal.h"
#include "pipe_io.h"
#include "poll_scratch.h"
#include "record_lock_descriptors.h"
#include "socket_internal.h"

namespace gesher {
namespace {

// Descriptor-backed stdio flushes after application atexit handlers and static
// destructors, and still dispatches through LookupSocket. Keep this registry
// alive until process teardown: destroying it first loses live socket owners
// and makes that final flush enter an already-destroyed mutex. Native handles
// and any deliberately quarantined owners remain process-owned until exit.
struct SocketRegistry {
  std::mutex mutex;
  std::unordered_map<int, SocketFd> entries;
};
// Static storage preserves startup construction without a fallible allocation
// or a registered destructor. Map entries still use the ordinary allocator.
alignas(SocketRegistry) unsigned char socket_registry_storage[sizeof(SocketRegistry)];
SocketRegistry& socket_registry = *new (socket_registry_storage) SocketRegistry;
std::mutex& table_mutex = socket_registry.mutex;
std::unordered_map<int, SocketFd>& table = socket_registry.entries;
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

// Stateless logical devices live in the executable-owned description table,
// so all modules and explicitly inherited descriptors agree on their type.
bool LookupDevice(int fd, Device* device) {
  DescriptorKind kind = GetDescriptorKind(fd);
  if (kind != DescriptorKind::kZero && kind != DescriptorKind::kRandom) return false;
  *device = kind == DescriptorKind::kZero ? Device::kZero : Device::kRandom;
  return true;
}

void ForgetDevice(int) {}  // retiring the OFD retires its logical device kind

int ReadDevice(Device device, void* buf, unsigned int count) {
  if (count == 0) return 0;
  if (!buf) return errno = EFAULT, -1;
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
  else if (SetDescriptorKind(fd, DescriptorKind::kUnshareable) < 0) {
    int error = errno;
    _close(fd);
    return errno = error, -1;
  }
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
      ForgetOfd(fd);
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
  // The OFD was allocated by FdFromHandle before this descriptor is published.
  SetDescriptorKind(fd, device == Device::kZero ? DescriptorKind::kZero : DescriptorKind::kRandom);
}

bool DeviceFromFd(int fd, Device* device) { return LookupDevice(fd, device); }

void ForgetStreamDevice(int fd, bool closed_or_replaced) {
  Device device;
  if (!LookupDevice(fd, &device)) return;
  int saved = errno;
  if (closed_or_replaced || HandleFromFd(fd) == INVALID_HANDLE_VALUE) ForgetOfd(fd);
  errno = saved;
}

}  // namespace gesher

using gesher::Device;
using gesher::SocketFd;

namespace {
void ReleaseCanceledSnapshot(void* raw) {
  static_cast<gesher::DescriptorSnapshotPtr*>(raw)->reset();
}
bool NativePipeIo(int fd, void* buffer, unsigned int count, bool writing, int* result) {
  gesher::DescriptorSnapshotPtr snapshot;
  {
    gesher::record_locks::DescriptorGuard descriptors;
    HANDLE file = gesher::HandleFromFd(fd);
    if (file == INVALID_HANDLE_VALUE) { *result = (errno = EBADF, -1); return true; }
    if (GetFileType(file) != FILE_TYPE_PIPE) return false;
    snapshot = gesher::CaptureDescriptor(fd);
  }
  if (!snapshot) { *result = -1; return true; }
  // Validate status and transfer through one capture. Looking the fd up again
  // could select a replacement, and would duplicate every pipe query/pin.
  pthread_cleanup_push(ReleaseCanceledSnapshot, &snapshot);
  *result = writing ? gesher::WriteFileDescription(snapshot, buffer, count)
                    : gesher::ReadFileDescription(snapshot, buffer, count);
  pthread_cleanup_pop(0);
  return true;
}
// The caller owns an exact native object reference for the whole operation.
// Append is one kernel write-to-end request; no seek/write race is possible
// between independent opens or writers in another process.
int TransferFileHandle(HANDLE file, void* buffer, unsigned int count, bool writing, bool append) {
  if (writing && count == 0) return 0;
  HANDLE event = nullptr;
  OVERLAPPED operation{};
  if (append) {
    event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return gesher::FailWithLastError();
    operation.hEvent = event;
    operation.Offset = operation.OffsetHigh = MAXDWORD;
  }
  const DWORD want = std::min<DWORD>(count, 0x7ffff000u);
  DWORD transferred = 0;
  BOOL done = writing ? WriteFile(file, buffer, want, &transferred, append ? &operation : nullptr)
                      : ReadFile(file, buffer, want, &transferred, nullptr);
  DWORD error = done ? ERROR_SUCCESS : GetLastError();
  if (append && !done && error == ERROR_IO_PENDING) {
    done = GetOverlappedResult(file, &operation, &transferred, TRUE);
    error = done ? ERROR_SUCCESS : GetLastError();
  }
  int native_error = 0;
  if (!done && !writing && error == ERROR_ACCESS_DENIED) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) native_error = EISDIR;
  }
  if (done && writing && transferred != 0) gesher::InotifyNoteWrite(file);
  if (event) CloseHandle(event);
  if (!done && !writing && (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)) return 0;
  if (!done) {
    return errno = native_error ? native_error :
        error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER ? EFAULT : gesher::ErrnoFromWin32(error), -1;
  }
  return static_cast<int>(transferred);
}

// Capture the object and shared description in one descriptor transaction.
// Native inspection runs after release and cannot select a recycled fd.
bool NativeFileIo(int fd, void* buffer, unsigned int count, bool writing, int* result) {
  gesher::DescriptorSnapshotPtr snapshot;
  {
    gesher::record_locks::DescriptorGuard descriptor_guard;
    HANDLE source = gesher::HandleFromFd(fd);
    if (source == INVALID_HANDLE_VALUE) { *result = (errno = EBADF, -1); return true; }
    if (GetFileType(source) != FILE_TYPE_DISK) return false;
    snapshot = gesher::CaptureDescriptor(fd);
  }
  if (!snapshot) { *result = -1; return true; }
  gesher::FileOperationLease operation(snapshot);
  if (operation.Error()) { *result = (errno = operation.Error(), -1); return true; }
  const int flags = gesher::FileDescriptionFlags(snapshot);
  if (flags < 0) { *result = -1; return true; }
  if ((flags & O_PATH) != 0 ||
      (flags & O_ACCMODE) == (writing ? O_RDONLY : O_WRONLY)) {
    *result = (errno = EBADF, -1);
    return true;
  }
  // A zero-length append must not move the shared cursor to EOF.
  if (writing && count == 0) { *result = 0; return true; }
  *result = TransferFileHandle(snapshot->handle, buffer, count, writing, writing && (flags & O_APPEND) != 0);
  return true;
}

int CloseFdBody(int fd, bool unpublished, SocketFd* deferred = nullptr) {
  gesher::ForgetEpollEntries(fd);
  SocketFd state;
  int result;
  bool last = false;
  if (!gesher::Remove(fd, &state, &last)) {
    gesher::ForgetDevice(fd);
    gesher::ForgetEventObject(fd);
    gesher::ForgetBinder(fd);
    // A pipe's last writer closing makes its reader readable (end of file).
    const bool pipe = gesher::HasPollWaiters() &&
                      GetFileType(gesher::HandleFromFd(fd)) == FILE_TYPE_PIPE;
    result = gesher::CrtResult(_close(fd));
    const int close_errno = errno;
    if (result == 0 || gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) {
      gesher::ForgetOfd(fd);
    }
    errno = close_errno;
    if (pipe) gesher::WakePollers();
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
    const int crt_errno = errno;
    if (crt_closed == 0 || gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) {
      gesher::ForgetOfd(fd);
    }
    if (unpublished && crt_closed < 0) return errno = crt_errno, -1;
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
void RetiredDescriptor::Retire(int fd) {
  // CRT has released the old handle while its fd number is still protected
  // from participating reuse. Retire precisely the virtual owners belonging
  // to that old slot, before a replacement is published.
  ForgetEpollEntries(fd);
  SocketFd socket;
  bool last = false;
  if (Remove(fd, &socket, &last)) {
    if (last) {
      std::lock_guard<std::mutex> lock(socket.description->error_mutex);
      socket.description->native_closing = true;
      socket_ = socket.socket;
      description_ = std::move(socket.description);
    }
  } else {
    ForgetDevice(fd);
    ForgetEventObject(fd);
    ForgetBinder(fd);
  }
  ForgetOfd(fd);
  if (HasPollWaiters()) WakePollers();
}
int RetiredDescriptor::Finish() {
  if (!description_) return 0;
  auto description = std::move(description_);
  // Linger/operation retirement may block. No descriptor or stdio registry
  // transaction is held here. Failed close retains the existing quarantine.
  return ws::Close(socket_, description, true);
}
RetiredDescriptor::~RetiredDescriptor() {
  const int saved = errno;
  Finish();
  errno = saved;
}
int ReadFileDescription(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count) {
  FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const int flags = FileDescriptionIoFlags(snapshot);
  if (flags < 0) return -1;
  if ((flags & O_PATH) != 0 || (flags & O_ACCMODE) == O_WRONLY) return errno = EBADF, -1;
  if (snapshot->kind == DescriptorKind::kUnshareable) return errno = EOPNOTSUPP, -1;
  count = std::min(count, 0x7ffff000u);
  if (snapshot->kind == DescriptorKind::kZero || snapshot->kind == DescriptorKind::kRandom) {
    return ReadDevice(snapshot->kind == DescriptorKind::kZero ? Device::kZero : Device::kRandom, bytes, count);
  }
  if (GetFileType(snapshot->handle) == FILE_TYPE_PIPE)
    return TransferPipe(snapshot, bytes, count, false);
  if (count == 0 && GetFileType(snapshot->handle) != FILE_TYPE_DISK) return 0;
  return TransferFileHandle(snapshot->handle, bytes, count, false, false);
}
int WriteFileDescription(const DescriptorSnapshotPtr& snapshot, const void* bytes, unsigned count) {
  FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const int flags = FileDescriptionIoFlags(snapshot);
  if (flags < 0) return -1;
  if ((flags & O_PATH) != 0 || (flags & O_ACCMODE) == O_RDONLY) return errno = EBADF, -1;
  if (snapshot->kind == DescriptorKind::kUnshareable) return errno = EOPNOTSUPP, -1;
  count = std::min(count, 0x7ffff000u);
  if (snapshot->kind == DescriptorKind::kZero) return static_cast<int>(count);
  const bool append = (flags & O_APPEND) != 0 && GetFileType(snapshot->handle) == FILE_TYPE_DISK;
  int result = GetFileType(snapshot->handle) == FILE_TYPE_PIPE
      ? TransferPipe(snapshot, const_cast<void*>(bytes), count, true)
      : TransferFileHandle(snapshot->handle, const_cast<void*>(bytes), count, true, append);
  if (result > 0 && HasPollWaiters() && GetFileType(snapshot->handle) == FILE_TYPE_PIPE) WakePollers();
  return result;
}
}  // namespace gesher

extern "C" {

int gesher_is_socket(int fd) {
  SocketFd state;
  return gesher::LookupSocket(fd, &state) ? 1 : 0;
}

int gesher_is_device_fd(int fd) {
  Device device;
  return gesher::LookupDevice(fd, &device) ? 1 : 0;
}

int close(int fd) { return CloseDescriptor(fd); }

int read(int fd, void* buf, unsigned int count) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    count = std::min(count, 0x7ffff000u);
    int result;
    if (NativeFileIo(fd, buf, count, false, &result) || NativePipeIo(fd, buf, count, false, &result)) return result;
    int flags = gesher::GetFileStatusFlags(fd);
    if (flags < 0) return -1;
    if ((flags & O_PATH) != 0 || (flags & O_ACCMODE) == O_WRONLY) return errno = EBADF, -1;
    Device device;
    if (gesher::LookupDevice(fd, &device)) return gesher::ReadDevice(device, buf, count);
    if (gesher::ReadEventObject(fd, buf, count, &result)) return result;
    return gesher::CrtResult(_read(fd, buf, count));
  }
  gesher::ws::Buffer buffer = {buf, count};
  return static_cast<int>(gesher::ws::RecvOwned(state.socket, &buffer, 1, 0, nullptr, state.description));
}

int write(int fd, const void* buf, unsigned int count) {
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    count = std::min(count, 0x7ffff000u);
    int result;
    if (NativeFileIo(fd, const_cast<void*>(buf), count, true, &result) ||
        NativePipeIo(fd, const_cast<void*>(buf), count, true, &result)) return result;
    int flags = gesher::GetFileStatusFlags(fd);
    if (flags < 0) return -1;
    if ((flags & O_PATH) != 0 || (flags & O_ACCMODE) == O_RDONLY) return errno = EBADF, -1;
    if (gesher::WriteEventObject(fd, buf, count, &result)) return result;
    result = gesher::CrtResult(_write(fd, buf, count));
    if (result > 0) gesher::InotifyNoteWrite(fd);
    if (result > 0 && gesher::HasPollWaiters() &&
        GetFileType(gesher::HandleFromFd(fd)) == FILE_TYPE_PIPE) {
      gesher::WakePollers();
    }
    return result;
  }
  gesher::ws::Buffer buffer = {const_cast<void*>(buf), count};
  return static_cast<int>(gesher::ws::SendOwned(state.socket, &buffer, 1, 0, nullptr, state.description));
}

int dup(int fd) {
  gesher::record_locks::DescriptorGuard descriptor_guard;
  // Resolve/allocate the source description before publishing any new fd.
  if (!gesher::CaptureDescriptor(fd)) return -1;
  SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) {
    int fd2 = gesher::CrtResult(_dup(fd));
    Device device;
    if (fd2 >= 0 && gesher::LookupDevice(fd, &device)) gesher::RegisterDevice(fd2, device);
    if (fd2 >= 0) gesher::DupEventObject(fd, fd2);
    if (fd2 >= 0) gesher::DupBinder(fd, fd2);
    if (fd2 >= 0) {
      int error = gesher::DupOfd(fd, fd2) < 0 ? errno : 0;
      HANDLE h2 = gesher::HandleFromFd(fd2);
      if (!error && !SetHandleInformation(h2, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        error = gesher::ErrnoFromWin32(GetLastError());
      }
      if (error) {
        gesher::CloseUnpublishedDuplicate(fd2);
        return errno = error, -1;
      }
    }
    return fd2;
  }
  // WSADuplicateSocket creates distinct native connect-error ownership.
  // POSIX aliases instead share the same native socket and last-close count.
  int fd2 = gesher::RegisterSocket(state);
  if (fd2 >= 0 && gesher::DupOfd(fd, fd2) < 0) {
    int error = errno;
    gesher::CloseUnpublishedDuplicate(fd2);
    return errno = error, -1;
  }
  return fd2;
}

static int DupTo(int fd, int fd2, bool cloexec) {
  SocketFd target;
  bool close_target = false;
  int saved, metadata_error;
  {
    gesher::record_locks::DescriptorGuard descriptor_guard;
    if (fd2 < 0 || fd2 >= 8192) return errno = EBADF, -1;
    if (fd == fd2) return gesher::HandleFromFd(fd) != INVALID_HANDLE_VALUE ? fd : (errno = EBADF, -1);
    if (!gesher::CaptureDescriptor(fd)) return -1;
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
    gesher::ForgetOfd(fd2);
    Device device;
    if (!source_is_socket && gesher::LookupDevice(fd, &device)) {
      gesher::RegisterDevice(fd2, device);
    } else if (!source_is_socket) {
      gesher::DupEventObject(fd, fd2);
      gesher::DupBinder(fd, fd2);
    }
    if (gesher::DupOfd(fd, fd2) < 0 && !metadata_error) metadata_error = errno;
    HANDLE h2 = gesher::HandleFromFd(fd2);
    if (h2 == INVALID_HANDLE_VALUE) {
      if (!metadata_error) metadata_error = EBADF;
    } else if (!SetHandleInformation(h2, HANDLE_FLAG_INHERIT,
                                     cloexec ? 0 : HANDLE_FLAG_INHERIT)) {
      // Do not publish a successful dup3 when close-on-exec was not applied.
      // Keep this inside the same descriptor transaction as replacement.
      if (!metadata_error) metadata_error = gesher::ErrnoFromWin32(GetLastError());
    }
  }
  // POSIX dup2 does not report the replaced endpoint's close error. A failed
  // native close still retains explicit ownership before success is published.
  if (close_target) gesher::ws::Close(target.socket, target.description, true);
  if (metadata_error) return errno = metadata_error, -1;
  errno = saved;
  return fd2;
}

int dup2(int fd, int fd2) { return DupTo(fd, fd2, false); }

int dup3(int oldfd, int newfd, int flags) {
  if (oldfd == newfd || (flags & ~O_CLOEXEC) != 0) return errno = EINVAL, -1;
  return DupTo(oldfd, newfd, (flags & O_CLOEXEC) != 0);
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

}  // namespace

namespace gesher {

void InitializeCrtDescriptorErrors() {
  _set_invalid_parameter_handler(IgnoreInvalidParameter);
}

}  // namespace gesher

namespace {

const bool invalid_parameter_handler_installed =
    (gesher::InitializeCrtDescriptorErrors(), true);

// Linux has no text mode: files the CRT opens without an explicit mode
// (fopen(path, "r"), _wopen without _O_TEXT) are binary, so reads return
// the file's bytes, CRs and Ctrl-Z included.
const bool binary_file_mode_set = (_set_fmode(_O_BINARY), true);

}  // namespace

// --- poll -----------------------------------------------------------------------

namespace {

constexpr short kIn = POLLIN | POLLRDNORM;
constexpr short kOut = POLLOUT | POLLWRNORM;

struct FdPollScratch {
  std::vector<gesher::ws::PollEntry> sockets;
  std::vector<nfds_t> socket_index;
  std::vector<std::shared_ptr<gesher::EventFd>> eventfds;
  std::vector<nfds_t> eventfd_index;
  std::vector<HANDLE> handles;

  void Reset() {
    gesher::ClearPollScratch(handles);
    gesher::ClearPollScratch(eventfd_index);
    gesher::ClearPollScratch(eventfds);
    gesher::ClearPollScratch(socket_index);
    gesher::ClearPollScratch(sockets);
  }
};

// Readiness of a non-socket fd: pipes are readable when data is buffered or
// the writer is gone; files and other devices are always ready. Returns false
// for a pipe that has nothing yet.
int PollFile(pollfd* p) {
  // The NUL fd reserves a number, but an epoll is readable only when its
  // interest list has a reportable event. It is never a writable file.
  if (gesher::PollEpoll(p->fd, p->events, &p->revents)) return p->revents != 0;
  gesher::DescriptorSnapshotPtr snapshot;
  {
    gesher::record_locks::DescriptorGuard guard;
    HANDLE h = gesher::HandleFromFd(p->fd);
    if (h == INVALID_HANDLE_VALUE) {
      p->revents = POLLNVAL;
      return true;
    }
    if (GetFileType(h) != FILE_TYPE_PIPE) {
      p->revents = p->events & (kIn | kOut);
      return true;
    }
    snapshot = gesher::CaptureDescriptor(p->fd);
  }
  if (!snapshot) return -1;
  if (gesher::PollPipeHandle(snapshot, p->events, &p->revents) < 0) return -1;
  return p->revents != 0;
}

// A thread's way to be woken in poll: an auto-reset event when it waits with
// WaitForMultipleObjects, a wake socket when it waits in WSAPoll. Shared, so
// that a registration a cancelled thread left behind stays valid.
struct PollWaiter {
  HANDLE event = nullptr;
  gesher::ws::Socket socket = gesher::ws::kInvalidSocket;
  uint16_t port = 0;
  bool with_socket = false;
  std::atomic<bool> pending{false};  // a wake datagram is on its way

  ~PollWaiter() {
    if (event != nullptr) CloseHandle(event);
    if (socket != gesher::ws::kInvalidSocket) gesher::ws::CloseWakeSocket(socket);
  }
};

std::mutex waiters_mutex;
std::vector<std::shared_ptr<PollWaiter>> waiters;
std::atomic<size_t> waiter_count{0};

// Registers the calling thread's waiter for one poll; empty if it has none
// (it is then left to the 10 ms looks).
class WaiterRegistration {
 public:
  WaiterRegistration(bool with_socket) {
    thread_local std::shared_ptr<PollWaiter> mine = std::make_shared<PollWaiter>();
    if (mine->event == nullptr) mine->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (with_socket && mine->socket == gesher::ws::kInvalidSocket) {
      mine->socket = gesher::ws::NewWakeSocket(&mine->port);
    }
    if (mine->event == nullptr || (with_socket && mine->socket == gesher::ws::kInvalidSocket)) return;
    std::lock_guard<std::mutex> lock(waiters_mutex);
    mine->with_socket = with_socket;
    // A datagram still queued from an earlier poll only wakes this one early.
    mine->pending.store(false);
    waiters.push_back(mine);
    waiter_count.store(waiters.size(), std::memory_order_release);
    waiter_ = mine.get();
  }
  ~WaiterRegistration() {
    if (waiter_ == nullptr) return;
    std::shared_ptr<PollWaiter> released;  // freed after the lock
    std::lock_guard<std::mutex> lock(waiters_mutex);
    for (auto& w : waiters) {
      if (w.get() != waiter_) continue;
      std::swap(w, waiters.back());
      released = std::move(waiters.back());
      waiters.pop_back();
      break;
    }
    waiter_count.store(waiters.size(), std::memory_order_release);
  }
  WaiterRegistration(const WaiterRegistration&) = delete;
  WaiterRegistration& operator=(const WaiterRegistration&) = delete;

  PollWaiter* get() const { return waiter_; }

 private:
  PollWaiter* waiter_ = nullptr;
};

}  // namespace

namespace gesher {

bool HasPollWaiters() { return waiter_count.load(std::memory_order_acquire) != 0; }

void WakePollers() {
  if (!HasPollWaiters()) return;
  std::lock_guard<std::mutex> lock(waiters_mutex);
  for (auto& w : waiters) {
    if (!w->with_socket) SetEvent(w->event);
    else if (!w->pending.exchange(true)) ws::Wake(w->port);
  }
}

static int PollFdsImpl(pollfd* fds, nfds_t nfds, int timeout, HANDLE interrupt, bool* interrupted, PollObserver observer, void* context,
                       ws::NativePollObserver native_observer, void* native_context) {
  ULONGLONG deadline = gesher::DeadlineAfter(timeout);
  PollScratchLease<FdPollScratch> lease(nfds <= kPollScratchLimit);
  auto& scratch = lease.get();
  auto& sockets = scratch.sockets;
  auto& socket_index = scratch.socket_index;
  // Held so that their events stay open while waited on.
  auto& eventfds = scratch.eventfds;
  auto& eventfd_index = scratch.eventfd_index;
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
    } else {
      int result = PollFile(p);
      if (result < 0) return -1;
      if (!result) waiting_files = true;
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
  // What has no wait handle a wait here can take (pipes; eventfds and the
  // interrupt next to sockets) wakes it through WakePollers. A write before
  // the registration woke nobody, so the first round only looks.
  std::optional<WaiterRegistration> registration;
  if (ready == 0 && timeout != 0 &&
      (interrupt != nullptr || waiting_files || (!sockets.empty() && !eventfds.empty()))) {
    registration.emplace(!sockets.empty());
  }
  PollWaiter* waiter = registration ? registration->get() : nullptr;
  bool look_first = waiter != nullptr;
  auto& handles = scratch.handles;
  for (;;) {
    int wait_ms = 0;  // with something ready, only collect what the rest has
    if (ready == 0 && !look_first) {
      if (timeout < 0) {
        wait_ms = -1;
      } else {
        wait_ms = gesher::MillisUntil(deadline);
      }
      if (sliced && (wait_ms < 0 || wait_ms > 10)) wait_ms = 10;
    }
    look_first = false;

    if (!sockets.empty()) {
      bool woken = false;
      const ws::Socket wake = waiter != nullptr && waiter->with_socket ? waiter->socket : ws::kInvalidSocket;
      const int sampled = ws::Poll(sockets.data(), sockets.size(), wait_ms, native_observer, native_context,
                                   wake, &woken);
      native_observer = nullptr;  // one real native sample, never a mutable global hook
      if (sampled < 0) return -1;
      if (woken) {
        waiter->pending.store(false);  // a later wake sends again
        ws::DrainWakeSocket(waiter->socket);
        if (interrupt != nullptr) *interrupted = true;  // rebuild an epoll's interest snapshot
      }
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
      if (waiter != nullptr) handles.push_back(waiter->event);
      if (interrupt != nullptr) handles.push_back(interrupt);  // last, as below
      if (handles.size() > MAXIMUM_WAIT_OBJECTS) {
        handles.clear();  // too many to wait for: look every 10 ms
        if (wait_ms < 0 || wait_ms > 10) wait_ms = 10;
        if (interrupt != nullptr) *interrupted = true;
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
        if (interrupt != nullptr && waiter != nullptr &&
            w == WAIT_OBJECT_0 + n - 2) *interrupted = true;
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
    if (interrupt != nullptr && waiter != nullptr && !waiter->with_socket &&
        WaitForSingleObject(waiter->event, 0) == WAIT_OBJECT_0) *interrupted = true;
    if (waiting_files) {
      waiting_files = false;
      for (nfds_t i = 0; i < nfds; ++i) {
        pollfd* p = &fds[i];
        SocketFd state;
        if (p->fd < 0 || p->revents != 0 || LookupSocket(p->fd, &state) || LookupEventFd(p->fd)) {
          continue;
        }
        int result = PollFile(p);
        if (result < 0) return -1;
        if (!result) waiting_files = true;
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
