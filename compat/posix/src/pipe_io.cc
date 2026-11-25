// Message-type kernel pipes with byte-stream reads preserve PIPE_BUF admission.
// Windows owns capacity, peer lifetime and waiter completion across processes.
#include "pipe_io.h"
#include "internal.h"
#include "record_lock_descriptors.h"
#include "ofd_internal.h"

#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <poll.h>
#include <pthread.h>
#include <winternl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace gesher {
namespace {
int QueryPipe(HANDLE file, void* value, ULONG size, FILE_INFORMATION_CLASS kind) {
  using Query = LONG(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FILE_INFORMATION_CLASS);
  using Error = ULONG(WINAPI*)(LONG);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto query = ntdll ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationFile"))) : nullptr;
  auto convert = ntdll ? reinterpret_cast<Error>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError"))) : nullptr;
  if (!query || !convert) return errno = ENOSYS, -1;
  IO_STATUS_BLOCK status{};
  LONG result = query(file, &status, value, size, kind);
  return result < 0 ? (errno = ErrnoFromWin32(convert(result)), -1) : 0;
}

class PipeAdmission {
 public:
  ~PipeAdmission() { if (mutex_) ReleaseMutex(mutex_); }
  int Acquire(const DescriptorSnapshotPtr& snapshot, bool waiting, HANDLE canceled = nullptr,
              bool nonblocking_io = false, bool require_known = true, HANDLE interrupted = nullptr) {
    auto& description = *snapshot->description;
    if (!description.OwnedPipe() || (require_known && !description.KnownPipeStatus()))
      return errno = EOPNOTSUPP, -1;
    for (;;) {
      if (canceled && WaitForSingleObject(canceled, 0) == WAIT_OBJECT_0) return errno = EINTR, -1;
      if (interrupted && WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0) return errno = EINTR, -1;
      DWORD wait = WaitForSingleObject(description.pipe_mutex, waiting ? 10 : 0);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        mutex_ = description.pipe_mutex;
        return !require_known || description.KnownPipeStatus() ? 0 : (errno = EOPNOTSUPP, -1);
      }
      if (wait != WAIT_TIMEOUT) return FailWithLastError();
      if (!waiting) return errno = nonblocking_io ? EAGAIN : EOPNOTSUPP, -1;
    }
  }
 private:
  HANDLE mutex_ = nullptr;
};

// I/O stays on the calling thread. A thread-pool wait runs a callback only
// when cancellation is requested; ordinary transfers allocate no worker stack.
struct NativePipeOperation {
  HANDLE file = nullptr, thread = nullptr;
  OVERLAPPED overlapped{};
  PTP_WAIT wait = nullptr, interrupt_wait = nullptr;
  std::atomic<bool> active{true};
  ~NativePipeOperation() {
    active.store(false, std::memory_order_release);
    for (PTP_WAIT callback : {wait, interrupt_wait}) {
      if (!callback) continue;
      SetThreadpoolWait(callback, nullptr, nullptr);
      WaitForThreadpoolWaitCallbacks(callback, TRUE);
      CloseThreadpoolWait(callback);
    }
    if (thread) CloseHandle(thread);
    if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
  }
};
void CALLBACK CancelNativePipe(PTP_CALLBACK_INSTANCE, void* raw, PTP_WAIT, TP_WAIT_RESULT) {
  auto* operation = static_cast<NativePipeOperation*>(raw);
  // Retry the entry race: ERROR_NOT_FOUND can mean ReadFile has not entered
  // the kernel yet. Retirement joins this callback before the caller may
  // issue another I/O operation or release the buffer/OVERLAPPED.
  while (operation->active.load(std::memory_order_acquire)) {
    CancelIoEx(operation->file, &operation->overlapped);
    CancelSynchronousIo(operation->thread);
    Sleep(1);
  }
}
int CompletePipeIo(BOOL done, DWORD transferred, DWORD error, bool writing) {
  if (done || error == ERROR_MORE_DATA) {
    if (writing && transferred == 0) return errno = EAGAIN, -1;
    return static_cast<int>(transferred);
  }
  if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
    return writing ? (errno = EPIPE, -1) : 0;
  if (error == ERROR_NO_DATA) return errno = writing ? EPIPE : EAGAIN, -1;
  if (error == ERROR_OPERATION_ABORTED) return errno = EINTR, -1;
  if (error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER) return errno = EFAULT, -1;
  return errno = ErrnoFromWin32(error), -1;
}
int NativePipeTransfer(HANDLE file, void* bytes, unsigned count, bool writing,
                       bool immediate, HANDLE canceled, HANDLE interrupted) {
  DWORD transferred = 0;
  if (immediate) {
    const BOOL done = writing ? WriteFile(file, bytes, count, &transferred, nullptr)
                              : ReadFile(file, bytes, count, &transferred, nullptr);
    return CompletePipeIo(done, transferred, done ? ERROR_SUCCESS : GetLastError(), writing);
  }
  NativePipeOperation operation;
  operation.file = file;
  operation.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.overlapped.hEvent) return FailWithLastError();
  if (canceled || interrupted) {
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
          &operation.thread, THREAD_TERMINATE, FALSE, 0)) return FailWithLastError();
    if (canceled) {
      operation.wait = CreateThreadpoolWait(CancelNativePipe, &operation, nullptr);
      if (!operation.wait) return FailWithLastError();
      SetThreadpoolWait(operation.wait, canceled, nullptr);
    }
    if (interrupted && interrupted != canceled) {
      operation.interrupt_wait = CreateThreadpoolWait(CancelNativePipe, &operation, nullptr);
      if (!operation.interrupt_wait) return FailWithLastError();
      SetThreadpoolWait(operation.interrupt_wait, interrupted, nullptr);
    }
  }
  // A valid, unique OVERLAPPED is legal for synchronous handles too. It also
  // lets an external asynchronous pipe complete without relying on UCRT's
  // unspecified ReadFile(NULL) behavior for that handle type.
  BOOL done = writing ? WriteFile(file, bytes, count, &transferred, &operation.overlapped)
                       : ReadFile(file, bytes, count, &transferred, &operation.overlapped);
  DWORD error = done ? ERROR_SUCCESS : GetLastError();
  if (!done && error == ERROR_IO_PENDING) {
    done = GetOverlappedResult(file, &operation.overlapped, &transferred, TRUE);
    error = done ? ERROR_SUCCESS : GetLastError();
  }
  operation.active.store(false, std::memory_order_release);
  return CompletePipeIo(done, transferred, error, writing);
}
}

int SetPipeNonBlocking(HANDLE file, bool nonblocking) {
  // The caller owns endpoint admission, or the endpoint is unpublished.
  DWORD mode = PIPE_READMODE_BYTE | (nonblocking ? PIPE_NOWAIT : PIPE_WAIT);
  if (SetNamedPipeHandleState(file, &mode, nullptr, nullptr)) return 0;
  return GetLastError() == ERROR_PIPE_BUSY ? (errno = EOPNOTSUPP, -1) : FailWithLastError();
}

int CreatePipeDescriptors(int descriptors[2], int flags) {
  if (!descriptors) return errno = EFAULT, -1;
  unsigned char nonce[16];
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    return errno = EIO, -1;
  wchar_t name[100] = L"\\\\.\\pipe\\gesher-";
  constexpr wchar_t hex[] = L"0123456789abcdef";
  const size_t prefix = 16;
  for (size_t i = 0; i != sizeof(nonce); ++i) {
    name[prefix + 2 * i] = hex[nonce[i] >> 4];
    name[prefix + 2 * i + 1] = hex[nonce[i] & 15];
  }
  name[prefix + 2 * sizeof(nonce)] = 0;
  HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
  if (server == INVALID_HANDLE_VALUE) return FailWithLastError();
  HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
      0, nullptr, OPEN_EXISTING, 0, nullptr);
  int error = client == INVALID_HANDLE_VALUE ? ErrnoFromWin32(GetLastError()) : 0;
  if (!error && !ConnectNamedPipe(server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    error = ErrnoFromWin32(GetLastError());
  if (!error && (SetPipeNonBlocking(server, (flags & O_NONBLOCK) != 0) < 0 ||
                 SetPipeNonBlocking(client, (flags & O_NONBLOCK) != 0) < 0)) error = errno;
  // Restrict the published handles to their real directional data access.
  // Attribute access permits a later F_SETFL without granting opposite I/O.
  HANDLE handles[2] = {nullptr, nullptr};
  const DWORD access[2] = {FILE_READ_DATA, FILE_WRITE_DATA};
  const HANDLE endpoints[2] = {server, client};
  for (int i = 0; !error && i != 2; ++i) {
    if (!DuplicateHandle(GetCurrentProcess(), endpoints[i], GetCurrentProcess(), &handles[i],
          access[i] | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
          (flags & O_CLOEXEC) == 0, 0)) error = ErrnoFromWin32(GetLastError());
  }
  CloseHandle(server);
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  record_locks::DescriptorGuard guard;
  int slots[2] = {-1, -1};
  for (int i = 0; !error && i != 2; ++i) {
    slots[i] = _open_osfhandle(reinterpret_cast<intptr_t>(handles[i]), _O_BINARY |
        (i == 0 ? _O_RDONLY : _O_WRONLY) | ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0));
    if (slots[i] < 0) { error = gesher_errno_from_crt(errno); break; }
    handles[i] = nullptr;
    if (RegisterOfd(slots[i], (i == 0 ? O_RDONLY : O_WRONLY) | (flags & O_NONBLOCK)) < 0) error = errno;
    if (!error && InitializePipeDescription(CaptureDescriptor(slots[i]),
          (i == 0 ? O_RDONLY : O_WRONLY) | (flags & O_NONBLOCK)) < 0) error = errno;
  }
  if (error) {
    for (int i = 0; i != 2; ++i) {
      if (handles[i]) CloseHandle(handles[i]);
      if (slots[i] >= 0) { _close(slots[i]); ForgetOfd(slots[i]); }
    }
    return errno = error, -1;
  }
  descriptors[0] = slots[0];
  descriptors[1] = slots[1];
  return 0;
}

namespace {
int TransferPipeBody(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count,
                     bool writing, HANDLE canceled, HANDLE interrupted) {
  if (canceled && WaitForSingleObject(canceled, 0) == WAIT_OBJECT_0) return errno = EINTR, -1;
  if (interrupted && WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0) return errno = EINTR, -1;
  auto& description = *snapshot->description;
  PipeAdmission admission;
  bool immediate = false;
  if (description.OwnedPipe()) {
    const bool nonblocking = description.KnownPipeStatus() && (description.Flags() & O_NONBLOCK) != 0;
    if (admission.Acquire(snapshot, !nonblocking, canceled, nonblocking, false, interrupted) < 0) return -1;
    immediate = description.KnownPipeStatus() && (description.Flags() & O_NONBLOCK) != 0;
    if (writing && immediate && count > 4096) {
      FILE_PIPE_LOCAL_INFORMATION info{};
      if (QueryPipe(snapshot->handle, &info, sizeof(info), FilePipeLocalInformation) < 0) return -1;
      // Quota is a native admission hint, not a byte count: pending reads
      // reserve quota too. Let the actual write decide EAGAIN/peer failure.
      const unsigned amount = info.WriteQuotaAvailable ? info.WriteQuotaAvailable : 4096;
      count = std::min(count, amount);
    }
  }
  return NativePipeTransfer(snapshot->handle, bytes, count, writing, immediate, canceled, interrupted);
}
}

static int TransferPipeWithInterrupt(const DescriptorSnapshotPtr& snapshot, void* bytes,
                                      unsigned count, bool writing, HANDLE interrupted) {
  if (!count) return 0;
  if (!bytes) return errno = EFAULT, -1;
  // winpthreads cancellation uses longjmp. Defer delivery until every native
  // request, wait callback and admission lock has retired. A disabled caller
  // gets no cancellation callback and retains its pending cancellation.
  int previous = PTHREAD_CANCEL_DISABLE;
  int error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
  if (error) return errno = gesher_errno_from_crt(error), -1;
  HANDLE canceled = previous == PTHREAD_CANCEL_ENABLE ? static_cast<HANDLE>(pthread_getevent()) : nullptr;
  int result;
  if (previous == PTHREAD_CANCEL_ENABLE && !canceled) result = (errno = ENOMEM, -1);
  else result = TransferPipeBody(snapshot, bytes, count, writing, canceled, interrupted);
  const int saved = errno;
  pthread_setcancelstate(previous, nullptr);
  pthread_testcancel();
  errno = saved;
  return result;
}

int TransferPipe(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count, bool writing) {
  return TransferPipeWithInterrupt(snapshot, bytes, count, writing, nullptr);
}

int ReadPipeDescriptionInterruptible(const DescriptorSnapshotPtr& snapshot, void* bytes,
                                     unsigned count, HANDLE interrupted) {
  const int flags = FileDescriptionIoFlags(snapshot);
  if (flags < 0) return -1;
  if ((flags & O_PATH) != 0 || (flags & O_ACCMODE) == O_WRONLY) return errno = EBADF, -1;
  if (snapshot->kind != DescriptorKind::kFile || GetFileType(snapshot->handle) != FILE_TYPE_PIPE)
    return errno = EOPNOTSUPP, -1;
  return TransferPipeWithInterrupt(snapshot, bytes, std::min(count, 0x7ffff000u), false, interrupted);
}

int PollPipeHandle(const DescriptorSnapshotPtr& snapshot, short events, short* ready) {
  *ready = 0;
  PipeAdmission admission;
  if (admission.Acquire(snapshot, false) < 0) return -1;
  const int flags = snapshot->description->Flags();
  FILE_PIPE_LOCAL_INFORMATION info{};
  if (QueryPipe(snapshot->handle, &info, sizeof(info), FilePipeLocalInformation) < 0) return -1;
  short result = 0;
  const bool disconnected = info.NamedPipeState != 3;
  if ((flags & O_ACCMODE) != O_WRONLY) {
    if (info.ReadDataAvailable) result |= events & (POLLIN | POLLRDNORM);
    if (disconnected) result |= POLLHUP;
  }
  if ((flags & O_ACCMODE) != O_RDONLY) {
    if (disconnected) result |= POLLERR;
    else if (info.WriteQuotaAvailable >= 4096) result |= events & (POLLOUT | POLLWRNORM);
  }
  *ready = result;
  return 0;
}

int PipeBytesAvailable(const DescriptorSnapshotPtr& snapshot, int* bytes) {
  if (!bytes) return errno = EFAULT, -1;
  PipeAdmission admission;
  if (admission.Acquire(snapshot, false) < 0) return -1;
  // The writer's quota is not the queue length, especially while a reader
  // is pending or after its last peer closes. Do not manufacture FIONREAD.
  if ((snapshot->description->Flags() & O_ACCMODE) == O_WRONLY) return errno = EOPNOTSUPP, -1;
  FILE_PIPE_LOCAL_INFORMATION info{};
  if (QueryPipe(snapshot->handle, &info, sizeof(info), FilePipeLocalInformation) < 0) return -1;
  *bytes = static_cast<int>(std::min<ULONG>(info.ReadDataAvailable, 0x7fffffffu));
  return 0;
}
}
