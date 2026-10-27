#include "record_lock_duplicate.h"
#include "record_lock_descriptors.h"
#include "internal.h"
#include "socket_internal.h"
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

namespace gesher::record_locks {
namespace {
// The pinned UCRT's documented low-I/O ceiling, also exposed by bionic's
// _SC_OPEN_MAX. The Linux command worker sets its actual RLIMIT_NOFILE to
// this same bound; this is not the unrelated512 FILE-stream default.
// https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmaxstdio
constexpr size_t kDescriptorCapacity = 8192;
enum class PrivateKind { kUnused, kNul, kNativeNul, kUnknownSource };
struct PrivateDescriptor {
  PrivateKind kind = PrivateKind::kUnused;
  int descriptor = -1;
  HANDLE native_handle = nullptr;
};
std::array<PrivateDescriptor, kDescriptorCapacity> private_descriptors{};
// Process-owned, protected and noninherited. A shared private NUL object
// identifies reservations across fd/HANDLE reuse; pathname/FileId is useless.
// No static destructor closes it during other threads' CRT/VM destruction.
HANDLE nul_identity = nullptr;
thread_local int publishing_target = -1;

int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }
int EnsureNulIdentity() {
  if (!nul_identity) {
    HANDLE candidate = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (candidate == INVALID_HANDLE_VALUE) return WinError();
    // Retain ownership even if flag setup fails; do not forget a live handle.
    nul_identity = candidate;
  }
  if (!SetHandleInformation(nul_identity, HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE,
                            HANDLE_FLAG_PROTECT_FROM_CLOSE)) return WinError();
  return 0;
}

int DisposeNul(PrivateDescriptor* owned, const DuplicateOperations* operations = nullptr) {
  const bool native = owned->kind == PrivateKind::kNativeNul;
  HANDLE current = native ? owned->native_handle : gesher::HandleFromFd(owned->descriptor);
  if (current == INVALID_HANDLE_VALUE) { *owned = {}; return 0; }
  if (native) {
    DWORD flags;
    if (!GetHandleInformation(current, &flags)) {
      if (GetLastError() == ERROR_INVALID_HANDLE) { *owned = {}; return 0; }
      return WinError();
    }
  }
  bool same = false;
  int error = SameDescription(nul_identity, current, &same);
  if (error) return error;  // Unknown identity is retained, never closed.
  if (!same) { *owned = {}; return 0; }  // Already closed/reused: leave foreign fd untouched.
  // Only a private NUL FILE_OBJECT is disposed here. This is neither an
  // application any-close nor a source-file/OFD alias, and has no virtual fd
  // bookkeeping. A failure is retained for identity checking on the next call.
  if (native) {
    if (!CloseHandle(current)) return WinError();
  } else {
    int error = 0;
    if (operations && operations->close_nul) error = operations->close_nul(owned->descriptor);
    else if (_close(owned->descriptor) < 0) error = gesher_errno_from_crt(errno);
    if (error) return error;
  }
  *owned = {};
  return 0;
}

int DisposeAllNuls(int except = -1, const DuplicateOperations* operations = nullptr) {
  int first_error = 0;
  for (auto& owned : private_descriptors) if (owned.kind == PrivateKind::kNativeNul ||
      (owned.kind == PrivateKind::kNul && owned.descriptor != except)) {
    int error = DisposeNul(&owned, operations);
    if (error && !first_error) first_error = error;
  }
  return first_error;
}

void RetainUnknownSource(PrivateDescriptor* owned, int error) {
  // An unexpected publication/close failure can be ambiguous about whether
  // this fd was freed and reused by an unguarded raw publisher. NEVER retry
  // its number, even if a reused fd names the same source FILE_OBJECT.
  owned->kind = PrivateKind::kUnknownSource;
  std::fprintf(stderr, "gesher: private duplicate cleanup uncertain fd=%d errno=%d; retained, no fd-number retry\n",
               owned->descriptor, error);
}

int Reserve(int minimum, PrivateDescriptor** target, const DuplicateOperations* operations) {
  *target = nullptr;
  int error = DrainPrivateReservations();
  if (error) return error;
  error = EnsureNulIdentity();
  if (error) return error;
  for (size_t attempt = 0; attempt < kDescriptorCapacity; ++attempt) {
    auto& owned = private_descriptors[attempt];
    if (owned.kind != PrivateKind::kUnused) return ENOLCK;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), nul_identity, GetCurrentProcess(), &duplicate,
                          0, FALSE, DUPLICATE_SAME_ACCESS)) { error = WinError(); break; }
    owned = {PrivateKind::kNativeNul, -1, duplicate};  // Ownership precedes fallible CRT adoption.
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
      error = gesher_errno_from_crt(errno);
      break;
    }
    owned = {PrivateKind::kNul, fd, nullptr};
    if (fd >= minimum) {
      error = DisposeAllNuls(fd, operations);
      if (!error) { *target = &owned; return 0; }
      break;
    }
  }
  if (!error) error = EMFILE;
  int cleanup = DisposeAllNuls(-1, operations);
  return cleanup ? cleanup : error;
}
}  // namespace

int DrainPrivateReservations() {
  for (const auto& owned : private_descriptors) if (owned.kind == PrivateKind::kUnknownSource) return EIO;
  // Actual public dup2 enters ClosingDescriptor recursively under this same
  // descriptor guard. Its still-occupied NUL target must not be drained.
  return DisposeAllNuls(publishing_target);
}

PrivateReservationStatus InspectPrivateReservations() {
  DescriptorGuard guard;
  PrivateReservationStatus status{};
  for (const auto& owned : private_descriptors) {
    status.nul += owned.kind == PrivateKind::kNul;
    status.native_nul += owned.kind == PrivateKind::kNativeNul;
    status.uncertain += owned.kind == PrivateKind::kUnknownSource;
  }
  return status;
}

int DuplicateAtLeast(int descriptor, int minimum, bool close_on_exec, const DuplicateOperations* operations) {
  const int saved_errno = errno;
  DescriptorGuard guard;
  HANDLE source = gesher::HandleFromFd(descriptor);
  if (source == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (minimum < 0 || minimum >= static_cast<int>(kDescriptorCapacity)) return errno = EINVAL, -1;
  PrivateDescriptor* target = nullptr;
  int error = Reserve(minimum, &target, operations);
  if (error) return errno = error, -1;
  const int fd = target->descriptor;
  // The target is an actual occupied, noninherited NUL reservation. Every
  // lower private fd was successfully disposed BEFORE source duplication.
  // Concurrent raw CRT allocation therefore cannot steal this target slot.
  publishing_target = fd;
  int moved = dup2(descriptor, fd);
  publishing_target = -1;
  if (moved < 0) {
    error = errno;
    HANDLE current = gesher::HandleFromFd(fd);
    bool same = false;
    int identity = current == INVALID_HANDLE_VALUE ? 0 : SameDescription(source, current, &same);
    if (identity || same) RetainUnknownSource(target, identity ? identity : error);
    else {
      int cleanup = DisposeNul(target);  // Also preserves an already-reused foreign fd.
      if (cleanup) error = cleanup;
    }
    return errno = error, -1;
  }
  HANDLE published = gesher::HandleFromFd(fd);
  gesher::SocketFd socket{};
  const bool source_is_socket = gesher::LookupSocket(descriptor, &socket);
  // This is final-state flag handling, NOT atomic F_DUPFD_CLOEXEC versus an
  // overlapping uncoordinated CreateProcess. The actual CRT dup2 may create
  // an inheritable source alias before this flag change. Atomic spawn/handle
  // publication remains a mandatory separate acceptance gate.
  // Existing socket policy is stricter: F_GETFD reports CLOEXEC and actual
  // SOCKET transport is never inherited. Do not export only its meaningless
  // CRT NUL placeholder when plain F_DUPFD is requested.
  const bool inherit = !close_on_exec && !source_is_socket;
  int flag_error = 0;
  if (operations && operations->set_inherit) flag_error = operations->set_inherit(fd, published, inherit);
  else if (!SetHandleInformation(published, HANDLE_FLAG_INHERIT, inherit ? HANDLE_FLAG_INHERIT : 0)) flag_error = WinError();
  if (flag_error) {
    error = flag_error;
    // This is still our just-created, unpublished alias. Close it ONCE with
    // complete virtual-resource cleanup, preserving user POSIX locks. Any error is ambiguous;
    // retain/account it and refuse later ownership-changing operations.
    int cleanup = 0;
    if (operations && operations->close_unpublished) cleanup = operations->close_unpublished(fd);
    else if (gesher::CloseUnpublishedDuplicate(fd) < 0) cleanup = errno;
    if (cleanup) RetainUnknownSource(target, cleanup);
    else *target = {};
    return errno = error, -1;
  }
  *target = {};  // Public ownership transfers only after the actual flag set.
  errno = saved_errno;
  return fd;
}
}  // namespace gesher::record_locks
