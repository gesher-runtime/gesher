// Message-type kernel pipes with byte-stream reads preserve PIPE_BUF admission.
// Windows owns capacity, peer lifetime and waiter completion across processes.
#include "pipe_io.h"
#include "fifo_internal.h"
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
#include <gesher/fork.h>
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
    const bool transfer = waiting || nonblocking_io;
    const auto still_nonblocking = [&description]() {
      return description.KnownPipeStatus() && (description.Flags() & O_NONBLOCK) != 0;
    };
    for (;;) {
      if (canceled && WaitForSingleObject(canceled, 0) == WAIT_OBJECT_0) return errno = EINTR, -1;
      if (interrupted && WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0) return errno = EINTR, -1;
      // Admission serializes calls on the file object. Contention between
      // immediate native transfers says nothing about pipe capacity.
      const DWORD wait = WaitForSingleObject(description.pipe_mutex, transfer ? 10 : 0);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        mutex_ = description.pipe_mutex;
        // An alias can set a new wait policy before we acquire the gate.
        if (nonblocking_io && !still_nonblocking()) return errno = EAGAIN, -1;
        return !require_known || description.KnownPipeStatus() ? 0 : (errno = EOPNOTSUPP, -1);
      }
      if (wait != WAIT_TIMEOUT) return FailWithLastError();
      if (!transfer) return errno = EOPNOTSUPP, -1;
      if (nonblocking_io && !still_nonblocking()) return errno = EAGAIN, -1;
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
  // A forked child has no thread pool (gesher/fork.h): its pipe transfers
  // block without asynchronous cancellation; they still end at EOF or data.
  if ((canceled || interrupted) && !InForkedChild()) {
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

// Waits a moment for a pipe's reader to free room, with nothing pending in
// the kernel: yields for the first tries (with the caller's checks of the
// pipe in between, a few hundred microseconds: a reader that keeps up frees
// room within that), then high-resolution timer waits of 50 us for the first
// half millisecond, growing to 1 ms until 20 ms have passed, then to 16 ms:
// a reader that pauses frees room within the short waits, and a writer whose
// reader has stopped reading costs next to no CPU (each wake checks the
// pipe). (Sleep(1) lasts a whole timer tick, about 16 ms, which would bound a
// writer that waits for room to a few hundred kilobytes a second.)
class RoomWait {
 public:
  RoomWait() = default;
  RoomWait(const RoomWait&) = delete;
  RoomWait& operator=(const RoomWait&) = delete;
  ~RoomWait() {
    if (timer_ != nullptr) CloseHandle(timer_);
  }
  void Reset() {
    waits_ = 0;
    waited_ = 0;
    last_ = 0;
  }
  void Pause() {
    constexpr unsigned kYields = 64;
    if (waits_ < kYields) {
      ++waits_;
      Sleep(0);
      return;
    }
    unsigned micros;
    if (waited_ < 500) {
      micros = 50;
    } else if (waited_ < 20000) {
      micros = std::min(1000u, std::max(100u, 2 * last_));
    } else {
      micros = std::min(16000u, 2 * last_);
    }
    last_ = micros;
    waited_ = std::min(waited_ + micros, 1u << 30);
    if (timer_ == nullptr && !tried_) {
      tried_ = true;
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    }
    if (timer_ != nullptr) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(micros) * 10;  // relative, in 100 ns units
      if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
          WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0) {
        return;
      }
    }
    Sleep(std::max(1u, micros / 1000));
  }

 private:
  HANDLE timer_ = nullptr;
  bool tried_ = false;
  unsigned waits_ = 0;   // yields so far
  unsigned waited_ = 0;  // microseconds of timer waits asked for so far
  unsigned last_ = 0;    // the last timer wait, in microseconds
};

// A blocking write to a Gesher pipe, so that no write of it is ever pending in
// the kernel with more than one byte. A pending write larger than the room in
// the pipe is partly read while it waits, a cancelled one (a ptrace stop, a
// signal) then reports none of it although some of it was read, and the
// thread that issued it holds the handle's file object (and the
// description's gate) until it completes, also while it is stopped. So, with
// the gate keeping every other Gesher user of the description out:
//  - when the free buffer space the pipe reports (WriteQuotaAvailable) holds
//    all of the write, it is written: it is buffered at once, complete;
//  - otherwise the reported room says little: a read pending on the other
//    end reserves its own size from it (a read of R bytes leaves 65536 - R,
//    none from 65536 on), although a write into that read completes at once.
//    The endpoint is switched to PIPE_NOWAIT (one file object for every
//    handle and process of the description), which Windows allows only while
//    the pipe holds no data. When it does, the pipe is empty: the write goes
//    in at once, all or nothing, into the pending read and the buffer (which
//    together take about R + 65536 bytes: beyond PIPE_BUF a piece of that
//    size first, halved until it fits), and the endpoint is switched back;
//  - when it does not (ERROR_PIPE_BUSY), data is waiting for a reader that
//    is not reading, and the reported room is real. Beyond PIPE_BUF, a piece
//    that fits it is written, and with no room at all one byte waits in a
//    blocking write, which completes when the reader frees room and which a
//    ptrace stop or a signal cancels without ambiguity (it was read or it was
//    not). A write of at most PIPE_BUF bytes is never split: it waits with
//    nothing pending (RoomWait, where a stop can take it) and tries again.
// `*total` counts the bytes in the pipe so far, across interruptions.
// Returns the total when done, -1 with errno, or kPipeTransferRestart when a
// ptrace stop interrupted it (the caller waits for the stop to end without
// the gate and continues with `*total`).
int WriteInPieces(HANDLE file, const char* bytes, unsigned count, unsigned* total,
                  HANDLE canceled, HANDLE interrupted) {
  constexpr unsigned kPipeBuf = 4096;
  constexpr unsigned kBuffer = 65536;       // the pipe's buffer (its write quota)
  constexpr unsigned kMaxPiece = 1u << 20;  // the most one no-wait write tries
  const bool atomic = count <= kPipeBuf;
  auto partial = [&](int error) { return *total != 0 ? static_cast<int>(*total) : (errno = error, -1); };
  auto mode = [&](bool nowait) {
    DWORD value = PIPE_READMODE_BYTE | (nowait ? PIPE_NOWAIT : PIPE_WAIT);
    return SetNamedPipeHandleState(file, &value, nullptr, nullptr) != 0;
  };
  RoomWait wait;
  while (*total < count) {
    if (Signaled(canceled) || Signaled(interrupted)) return partial(EINTR);
    char* const next = const_cast<char*>(bytes) + *total;
    const unsigned remaining = count - *total;
    FILE_PIPE_LOCAL_INFORMATION info{};
    if (QueryPipe(file, &info, sizeof(info), FilePipeLocalInformation) < 0) return partial(errno);
    const bool connected = info.NamedPipeState == 3;
    const unsigned room = static_cast<unsigned>(info.WriteQuotaAvailable);
    if (connected && room >= remaining) {
      const int written = NativePipeTransfer(file, next, remaining, true, true, nullptr, nullptr);
      if (written < 0) return partial(errno);
      *total += static_cast<unsigned>(written);
      wait.Reset();
      continue;
    }
    if (!connected) {
      // The reader is gone, or the pipe is closing: the write reports it.
      const int one = NativePipeTransfer(file, next, 1, true, false, canceled, interrupted);
      if (one == kPipeTransferRestart) return kPipeTransferRestart;
      if (one < 0) return partial(errno);
      *total += static_cast<unsigned>(one);
      continue;
    }
    if (mode(true)) {
      // The pipe is empty: the write goes in at once, all or nothing.
      // A pending read of R bytes leaves room 65536 - R and takes R bytes
      // besides the buffer; with none pending (room 65536) only the buffer
      // takes the write; a read of 65536 or more (room 0) takes at least that.
      const unsigned reach = room >= kBuffer ? kBuffer : 2 * kBuffer - room;
      unsigned piece = atomic ? remaining : std::min(std::min(remaining, reach), kMaxPiece);
      int written;
      for (;;) {
        // A no-wait write that does not fit moves nothing (EAGAIN).
        written = NativePipeTransfer(file, next, piece, true, true, nullptr, nullptr);
        if (written < 0 && errno == EAGAIN) written = 0;
        if (written != 0 || atomic || piece == 1) break;
        piece /= 2;
      }
      const int saved = errno;
      if (written > 0) *total += static_cast<unsigned>(written);
      if (!mode(false)) return partial(ErrnoFromWin32(GetLastError()));
      if (written < 0) return partial(saved);
      if (written > 0) {
        wait.Reset();
        continue;
      }
      // Nothing fitted (the reader went meanwhile): wait as below.
    } else if (GetLastError() != ERROR_PIPE_BUSY) {
      return partial(ErrnoFromWin32(GetLastError()));
    } else if (!atomic && room != 0) {
      // Data waits for the reader, and the room is real: a piece that fits.
      const int written = NativePipeTransfer(file, next, room, true, true, nullptr, nullptr);
      if (written < 0) return partial(errno);
      *total += static_cast<unsigned>(written);
      wait.Reset();
      continue;
    } else if (!atomic) {
      // The pipe is full: one byte waits for room.
      const int one = NativePipeTransfer(file, next, 1, true, false, canceled, interrupted);
      if (one == kPipeTransferRestart) return kPipeTransferRestart;
      if (one < 0) return partial(errno);
      *total += static_cast<unsigned>(one);
      continue;
    }
    // A PIPE_BUF write waits for room for all of it, with nothing pending.
    gesher::ptrace::ParkedScope parked;
    if (parked.Interrupted()) return kPipeTransferRestart;
    wait.Pause();
    if (parked.Interrupted()) return kPipeTransferRestart;
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
  // A FIFO's placeholder carries no data: its bytes are in the shared ring.
  else if (snapshot->kind == DescriptorKind::kFifo)
    result = TransferFifo(snapshot, bytes, count, writing, canceled, interrupted);
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
  if ((snapshot->kind != DescriptorKind::kFile && snapshot->kind != DescriptorKind::kFifo) ||
      GetFileType(snapshot->handle) != FILE_TYPE_PIPE)
    return errno = EOPNOTSUPP, -1;
  return TransferPipeWithInterrupt(snapshot, bytes, std::min(count, 0x7ffff000u), false, interrupted);
}

int PollPipeHandle(const DescriptorSnapshotPtr& snapshot, short events, short* ready) {
  *ready = 0;
  if (snapshot->kind == DescriptorKind::kFifo) return PollFifo(snapshot, events, ready);
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
  if (snapshot->kind == DescriptorKind::kFifo) return FifoBytesAvailable(snapshot, bytes);
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
