// Message-type kernel pipes with byte-stream reads preserve PIPE_BUF admission.
// Windows owns capacity, peer lifetime and waiter completion across processes.
#include "pipe_io.h"
#include "internal.h"
#include "record_lock_descriptors.h"
#include "ofd_internal.h"

#include <gesher/ptrace_state.h>

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
  // An interrupted transfer that reports bytes moved returns them (a short
  // count), as Linux does; one that reports none is EINTR.
  if (error == ERROR_OPERATION_ABORTED) {
    return transferred != 0 ? static_cast<int>(transferred) : (errno = EINTR, -1);
  }
  if (error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER) return errno = EFAULT, -1;
  return errno = ErrnoFromWin32(error), -1;
}
// NativePipeTransfer's result when a ptrace stop interrupted the blocked
// transfer before it moved a byte: the caller restarts it (ADR 0093).
constexpr int kPipeTransferRestart = -2;
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
  BOOL done;
  DWORD error;
  bool restart = false;
  {
    // Blocked for data or room (in ReadFile/WriteFile itself for a
    // synchronous handle, else in GetOverlappedResult) while owning the
    // description's gate: a ptrace stop interrupts it (ADR 0093), as a Linux
    // stop interrupts a blocked read() or write(), so that the gate and the
    // handle are free while the process is stopped. A cancelled read has
    // taken no data; a blocking write that can be pending here is a single
    // byte (WriteInPieces), which a cancellation cannot split.
    gesher::ptrace::ParkedScope parked;
    parked.SetIo(file, &operation.overlapped);
    done = writing ? WriteFile(file, bytes, count, &transferred, &operation.overlapped)
                   : ReadFile(file, bytes, count, &transferred, &operation.overlapped);
    error = done ? ERROR_SUCCESS : GetLastError();
    if (!done && error == ERROR_IO_PENDING) {
      done = GetOverlappedResult(file, &operation.overlapped, &transferred, TRUE);
      error = done ? ERROR_SUCCESS : GetLastError();
    }
    // A transfer the ptrace guard cancelled: it is the only canceller
    // besides the caller's cancellation events, so an aborted transfer that
    // moved nothing, with neither event set, restarts (also one the guard
    // reached after its target request completed).
    restart = !done && error == ERROR_OPERATION_ABORTED && transferred == 0 &&
              !(canceled && WaitForSingleObject(canceled, 0) == WAIT_OBJECT_0) &&
              !(interrupted && WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0);
  }
  operation.active.store(false, std::memory_order_release);
  if (restart) return kPipeTransferRestart;
  return CompletePipeIo(done, transferred, error, writing);
}

bool Signaled(HANDLE event) { return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }

// A blocking write to a Gesher pipe, so that no write of it is ever pending in
// the kernel with more than one byte. A pending write larger than the room in
// the pipe is partly read while it waits, a cancelled one (a ptrace stop, a
// signal) then reports none of it although some of it was read, and the
// thread that issued it holds the handle's file object (and the
// description's gate) until it completes, also while it is stopped. So:
//  - the bytes go in pieces that fit the free buffer space the pipe reports
//    (WriteQuotaAvailable): such a write is buffered at once, complete;
//  - the pipe reports no room both when its buffer is full and when a read is
//    pending on its other end (its buffer is then empty). One byte is then
//    written blocking: it completes at once into the pending read, or when
//    the reader frees room; a ptrace stop or a signal cancels it without
//    ambiguity (it was read or it was not). The next piece fits the room
//    reported again;
//  - a write of at most PIPE_BUF bytes is written whole when the reported
//    room holds it, and otherwise waits (1 ms polls, nothing pending, where a
//    stop can take it) while the buffer is partly full; when the pipe reports
//    no room at all it starts with the blocking byte, and its rest follows
//    as above (atomic unless a stop or signal falls in between and another
//    writer of the same description writes meanwhile).
// (Switching the endpoint to PIPE_NOWAIT for each piece would not need the
// reported room, but Windows refuses that switch with ERROR_PIPE_BUSY while
// the pipe holds data, which is when a blocking write needs it.)
// `*total` counts the bytes in the pipe so far, across interruptions.
// Returns the total when done, -1 with errno, or kPipeTransferRestart when a
// ptrace stop interrupted it (the caller waits for the stop to end without
// the gate and continues with `*total`).
int WriteInPieces(HANDLE file, const char* bytes, unsigned count, unsigned* total,
                  HANDLE canceled, HANDLE interrupted) {
  constexpr unsigned kPipeBuf = 4096;
  bool atomic = count <= kPipeBuf;
  auto partial = [&](int error) { return *total != 0 ? static_cast<int>(*total) : (errno = error, -1); };
  while (*total < count) {
    if (Signaled(canceled) || Signaled(interrupted)) return partial(EINTR);
    const unsigned remaining = count - *total;
    FILE_PIPE_LOCAL_INFORMATION info{};
    if (QueryPipe(file, &info, sizeof(info), FilePipeLocalInformation) < 0) return partial(errno);
    const unsigned room = static_cast<unsigned>(info.WriteQuotaAvailable);
    if (info.NamedPipeState == 3 && room != 0 && (!atomic || room >= remaining)) {
      const int written = NativePipeTransfer(file, const_cast<char*>(bytes) + *total,
                                             std::min(remaining, room), true, true, nullptr, nullptr);
      if (written < 0) return partial(errno);
      *total += static_cast<unsigned>(written);
      continue;
    }
    if (info.NamedPipeState == 3 && room != 0) {
      // A PIPE_BUF write and a partly full buffer: wait for room for all of
      // it, with nothing pending.
      gesher::ptrace::ParkedScope parked;
      if (parked.Interrupted()) return kPipeTransferRestart;
      Sleep(1);
      if (parked.Interrupted()) return kPipeTransferRestart;
      continue;
    }
    // No room reported (full, or a read pending on the other end), or the
    // reader is gone (the write reports it): one byte, blocking.
    const int one = NativePipeTransfer(file, const_cast<char*>(bytes) + *total, 1, true, false,
                                       canceled, interrupted);
    if (one == kPipeTransferRestart) return kPipeTransferRestart;
    if (one < 0) return partial(errno);
    *total += static_cast<unsigned>(one);
    atomic = false;  // its rest follows in pieces
  }
  return static_cast<int>(*total);
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
  unsigned written = 0;  // a blocking write's bytes already in the pipe
  for (;;) {
    if (Signaled(canceled) || Signaled(interrupted)) {
      return written != 0 ? static_cast<int>(written) : (errno = EINTR, -1);
    }
    int result;
    {
      auto& description = *snapshot->description;
      PipeAdmission admission;
      bool immediate = false;
      unsigned amount = count;
      if (description.OwnedPipe()) {
        const bool nonblocking = description.KnownPipeStatus() && (description.Flags() & O_NONBLOCK) != 0;
        if (admission.Acquire(snapshot, !nonblocking, canceled, nonblocking, false, interrupted) < 0) return -1;
        immediate = description.KnownPipeStatus() && (description.Flags() & O_NONBLOCK) != 0;
        if (writing && immediate && amount > 4096) {
          FILE_PIPE_LOCAL_INFORMATION info{};
          if (QueryPipe(snapshot->handle, &info, sizeof(info), FilePipeLocalInformation) < 0) return -1;
          // Quota is a native admission hint, not a byte count: pending reads
          // reserve quota too. Let the actual write decide EAGAIN/peer failure.
          const unsigned quota = info.WriteQuotaAvailable ? static_cast<unsigned>(info.WriteQuotaAvailable) : 4096u;
          amount = std::min(amount, quota);
        }
      }
      if (writing && !immediate && description.OwnedPipe() && description.KnownPipeStatus()) {
        result = WriteInPieces(snapshot->handle, static_cast<const char*>(bytes), count, &written,
                               canceled, interrupted);
      } else {
        result = NativePipeTransfer(snapshot->handle, bytes, amount, writing, immediate, canceled,
                                    interrupted);
      }
    }
    if (result != kPipeTransferRestart) return result;
    // A ptrace stop interrupted the blocked transfer: wait for the stop to
    // end without the gate, then go on (a write with the bytes it already
    // wrote), as Linux restarts the call (ADR 0093).
    gesher::ptrace::WaitWhileStopping();
  }
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
