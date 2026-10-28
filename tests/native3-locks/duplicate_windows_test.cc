// CI-only actual common-layer resources. Explicit callback failures below are
// internal unit dependencies, not evidence of real OS permission/close denial.
#include "../../compat/posix/src/record_lock_duplicate.h"
#include "../../compat/posix/src/record_lock_descriptors.h"
#include "../../compat/posix/src/record_lock_liveness.h"
#include "../../compat/posix/src/internal.h"
#include "../../compat/posix/src/socket_internal.h"
#include "../../compat/posix/src/event_internal.h"
#include "../../compat/posix/src/binder_internal.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <linux/android/binder.h>

namespace locks = gesher::record_locks;
namespace {
unsigned failures = 0, assertions = 0;
// Capacity/error controls intentionally inspect invalid descriptors. Let the
// UCRT report EBADF instead of terminating before those errno assertions.
void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}
int observed_target = -1, observed_nul = -1;
void Check(bool condition, const char* text) {
  ++assertions;
  if (!condition) ++failures;
  std::printf("%s duplicate ownership: %s\n", condition ? "PASS" : "FAIL", text);
  std::fflush(stdout);
}
bool Empty() {
  auto status = locks::InspectPrivateReservations();
  return !status.nul && !status.native_nul && !status.uncertain;
}
int DenyFlag(int fd, void*, bool) { observed_target = fd; return EACCES; }
int DenyNulClose(int fd) { if (observed_nul < 0) observed_nul = fd; return EIO; }
int DenyUnpublishedClose(int) { return EIO; }
std::atomic<int> reserved_target{-1};
std::atomic<bool> raw_ready{false}, raw_release{false};
void ActualReservationBarrier(int fd) {
  reserved_target.store(fd, std::memory_order_release);
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!raw_ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) std::this_thread::yield();
  Check(raw_ready.load(std::memory_order_acquire), "real raw allocator reached held-reservation phase");
}
DWORD Handles() {
  DWORD count = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &count)) std::_Exit(94);
  return count;
}
bool HandleParity(DWORD expected) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  unsigned stable = 0;
  do {
    stable = Handles() == expected ? stable + 1 : 0;
    if (stable == 3) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < end);
  return false;
}
bool InheritFlagIs(int fd, bool expected) {
  DWORD flags = 0;
  return GetHandleInformation(gesher::HandleFromFd(fd), &flags) && ((flags & HANDLE_FLAG_INHERIT) != 0) == expected;
}
int RawSentinel(const char* path) {
  HANDLE file = CreateFileW(gesher::Widen(path).c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return -1;
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  if (fd < 0) CloseHandle(file);
  return fd;
}
bool Sentinel(int fd) {
  char value = 0;
  return lseek(fd, 0, SEEK_SET) == 0 && read(fd, &value, 1) == 1 && value == 'S';
}
struct RawOwned { int descriptor = -1; HANDLE witness = nullptr; };
RawOwned RawWitness(const char* path) {
  RawOwned result;
  HANDLE file = CreateFileW(gesher::Widen(path).c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return result;
  if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &result.witness,
                        0, FALSE, DUPLICATE_SAME_ACCESS)) { CloseHandle(file); return result; }
  result.descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  if (result.descriptor < 0) { CloseHandle(file); CloseHandle(result.witness); result.witness = nullptr; }
  return result;
}
bool RawIdentity(const RawOwned& owned) {
  if (owned.descriptor < 0 || !owned.witness) return false;
  bool same = false;
  return locks::SameDescription(owned.witness, gesher::HandleFromFd(owned.descriptor), &same) == 0 && same;
}
bool RawCanary(const RawOwned& owned) {
  char byte = 0;
  return RawIdentity(owned) && _lseeki64(owned.descriptor, 0, SEEK_SET) == 0 &&
      _read(owned.descriptor, &byte, 1) == 1 && byte == 'S';
}
bool CloseRaw(RawOwned* owned) {
  bool closed = RawIdentity(*owned) && _close(owned->descriptor) == 0;
  if (owned->witness) closed = CloseHandle(owned->witness) && closed;
  *owned = {};
  return closed;  // A foreign/reused fd is NEVER closed by this cleanup.
}
bool Lock(int fd, int command, short kind, off_t start) {
  struct flock value{};
  value.l_type = kind; value.l_start = start; value.l_len = 10;
  return fcntl(fd, command, &value) == 0;
}
short Query(int fd, int command, off_t start) {
  struct flock value{};
  value.l_type = F_WRLCK; value.l_start = start; value.l_len = 10;
  if (fcntl(fd, command, &value) < 0) return -1;
  return value.l_type;
}
int Warm(int source) {
  int copy = fcntl(source, F_DUPFD_CLOEXEC, 32);
  Check(copy >= 32 && InheritFlagIs(copy, false), "real warm duplicate is noninherited");
  if (copy >= 0) Check(close(copy) == 0, "real warm duplicate closes");
  return copy;
}
struct Resources { int source = -1, peer = -1, query = -1; };
Resources OpenResource(const char* kind, const char* file, bool independent_query) {
  Resources value;
  if (!std::strcmp(kind, "regular")) {
    value.source = open(file, O_RDWR);
    if (independent_query) value.query = open(file, O_RDWR);
  } else if (!std::strcmp(kind, "device")) value.source = open("/dev/zero", O_RDONLY);
  else if (!std::strcmp(kind, "event")) value.source = eventfd(7, EFD_NONBLOCK | EFD_CLOEXEC);
  else if (!std::strcmp(kind, "socket")) {
    int pair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0) { value.source = pair[0]; value.peer = pair[1]; }
  } else if (!std::strcmp(kind, "binder")) value.source = open("/dev/binder", O_RDWR);
  return value;
}

void FlagRollback(const char* kind, const char* file, const char* sentinel) {
  auto resource = OpenResource(kind, file, true);
  int source = resource.source, peer = resource.peer, query = resource.query;
  Check(source >= 0, "actual source resource opened");
  if (!std::strcmp(kind, "regular")) Check(query >= 0, "actual independent regular query source opened");
  if (source < 0 || (!std::strcmp(kind, "regular") && query < 0)) {
    if (source >= 0) Check(close(source) == 0, "partial source setup closes");
    if (query >= 0) Check(close(query) == 0, "partial query setup closes");
    if (peer >= 0) Check(close(peer) == 0, "partial socket peer setup closes");
    return;
  }
  Warm(source);
  if (query >= 0) {
    Check(Lock(source, F_SETLK, F_WRLCK, 0), "real POSIX lock before private rollback");
    Check(Lock(source, F_OFD_SETLK, F_WRLCK, 100), "real OFD lock before private rollback");
  }
  DWORD before = Handles();
  locks::DuplicateOperations operations{};
  operations.set_inherit = DenyFlag;
  errno = 123;
  int result = locks::DuplicateAtLeast(source, 32, true, &operations);
  int error = errno;
  Check(result == -1 && error == EACCES && observed_target >= 32, "explicit flag-failure dependency returns actual failure");
  Check(Empty(), "all unpublished duplicate ownership retired");
  Check(_get_osfhandle(observed_target) == -1, "actual unpublished CRT fd closed");
  Check(HandleParity(before), "actual native handle count returns to warm baseline");
  gesher::SocketFd socket{};
  Check(!gesher::LookupSocket(observed_target, &socket), "no stale socket resource table entry");
  Check(!gesher::LookupEventFd(observed_target), "no stale event resource table entry");
  Check(!gesher::IsBinderFd(observed_target), "no stale Binder resource table entry");

  int raw = RawSentinel(sentinel);
  Check(raw >= 0, "actual independent raw publisher opened sentinel");
  if (raw >= 0) {
    if (raw != observed_target) {
      Check(_dup2(raw, observed_target) == 0, "actual known-free retired fd number reused by sentinel");
      Check(_close(raw) == 0, "raw staging sentinel closes");
      raw = observed_target;
    }
    Check(Sentinel(raw), "reused fd reads actual sentinel, never stale virtual redirection");
    Check(close(raw) == 0, "reused sentinel fd closes");
  }
  if (query >= 0) {
    Check(Query(query, F_OFD_GETLK, 0) == F_WRLCK, "internal rollback preserved source POSIX any-close locks");
    Check(Query(query, F_GETLK, 100) == F_WRLCK, "internal rollback preserved source OFD lock");
  } else if (!std::strcmp(kind, "device")) {
    unsigned char data[8]; std::memset(data, 0xff, sizeof(data));
    Check(read(source, data, sizeof(data)) == static_cast<ssize_t>(sizeof(data)) && data[0] == 0 && data[7] == 0, "original device dispatch remains live");
  } else if (!std::strcmp(kind, "event")) {
    uint64_t value = 0;
    Check(read(source, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)) && value == 7, "original event object remains live");
  } else if (!std::strcmp(kind, "socket")) {
    const char value = 'X';
    Check(write(source, &value, 1) == 1, "original actual SOCKET still sends");
    char observed = 0; bool received = false;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!received && std::chrono::steady_clock::now() < end) {
      int count = read(peer, &observed, 1);
      if (count == 1) received = true;
      else if (count < 0 && errno != EAGAIN) break;
      else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(received && observed == value, "original actual SOCKET peer receives after rollback");
    int plain = fcntl(source, F_DUPFD, 32);
    Check(plain >= 32 && fcntl(plain, F_GETFD, 0) == FD_CLOEXEC && InheritFlagIs(plain, false), "socket plain duplicate preserves existing never-inherit policy");
    if (plain >= 0) Check(close(plain) == 0, "socket plain duplicate resource closes");
  } else if (!std::strcmp(kind, "binder")) {
    uint32_t maximum = 1;
    Check(ioctl(source, BINDER_SET_MAX_THREADS, &maximum) == 0, "original Binder source completes actual broker roundtrip");
  }
  Check(close(source) == 0, "original source closes after rollback proof");
  if (query >= 0) {
    Check(Query(query, F_OFD_GETLK, 0) == F_UNLCK && Query(query, F_GETLK, 100) == F_UNLCK,
          "actual final user close releases POSIX and OFD ownership");
    Check(close(query) == 0, "independent query description closes");
  }
  if (peer >= 0) Check(close(peer) == 0, "original socket peer closes");
}

void RetainedNul(const char* file, const char* sentinel) {
  int source = open(file, O_RDWR), query = open(file, O_RDWR);
  Check(source >= 0 && query >= 0, "actual regular sources opened for retained private NUL");
  if (source < 0 || query < 0) {
    if (source >= 0) Check(close(source) == 0, "partial regular source setup closes");
    if (query >= 0) Check(close(query) == 0, "partial query setup closes");
    return;
  }
  Warm(source);
  Check(Lock(source, F_SETLK, F_WRLCK, 0), "POSIX lock before retained scratch dependency");
  Check(Lock(source, F_OFD_SETLK, F_WRLCK, 100), "OFD lock before retained scratch dependency");
  locks::DuplicateOperations operations{};
  operations.close_nul = DenyNulClose;
  int result = locks::DuplicateAtLeast(source, 32, false, &operations);
  Check(result == -1 && errno == EIO, "explicit private scratch-close failure remains failure");
  auto retained = locks::InspectPrivateReservations();
  Check(retained.nul > 0 && !retained.native_nul && !retained.uncertain && observed_nul >= 0,
        "every failed-close private NUL remains owned and counted");
  Check(InheritFlagIs(observed_nul, false), "actual retained private NUL handle is noninherited");
  Check(_close(observed_nul) == 0, "fixture deliberately closes identified retained private descriptor");
  int raw = RawSentinel(sentinel);
  Check(raw == observed_nul, "actual CRT publisher reuses retained fd number");
  Check(locks::DrainPrivateReservations() == 0 && Empty(), "identity-checked real cleanup drains only remaining private NULs");
  Check(Sentinel(raw), "reused foreign FILE_OBJECT survives private cleanup");
  Check(Query(query, F_OFD_GETLK, 0) == F_WRLCK && Query(query, F_GETLK, 100) == F_WRLCK,
        "retained NUL cleanup preserves actual POSIX/OFD source ownership");
  if (raw >= 0) Check(close(raw) == 0, "foreign sentinel closes after identity proof");
  Check(close(source) == 0 && close(query) == 0, "actual retained-scratch source descriptions close");
}

void Quarantine(const char* kind, const char* file, const char* sentinel) {
  auto resource = OpenResource(kind, file, false);
  int source = resource.source;
  Check(source >= 0, "actual source opened for uncertain rollback");
  if (source < 0) return;
  Warm(source);
  locks::DuplicateOperations operations{};
  operations.set_inherit = DenyFlag;
  operations.close_unpublished = DenyUnpublishedClose;
  Check(locks::DuplicateAtLeast(source, 32, true, &operations) == -1 && errno == EACCES,
        "explicit flag+cleanup failure preserves original errno");
  auto retained = locks::InspectPrivateReservations();
  Check(retained.uncertain == 1 && !retained.nul && !retained.native_nul, "uncertain source alias is explicitly retained and counted");
  Check(locks::DrainPrivateReservations() == EIO, "uncertain alias is never retried by reused fd number");
  bool same = false;
  Check(locks::SameDescription(gesher::HandleFromFd(source), gesher::HandleFromFd(observed_target), &same) == 0 && same,
        "actual uncertain target is a source FILE_OBJECT alias");
  gesher::SocketFd socket{};
  if (!std::strcmp(kind, "socket")) {
    gesher::ws::Address address{};
    Check(gesher::LookupSocket(observed_target, &socket) && gesher::ws::GetSockName(socket.socket, &address) == 0,
          "actual retained duplicate SOCKET remains live and explicitly table-owned");
  }
  if (!std::strcmp(kind, "event")) {
    uint64_t value = 0;
    Check(gesher::LookupEventFd(observed_target) && read(observed_target, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)) && value == 7,
          "actual retained event reference remains live and explicitly table-owned");
  }
  if (!std::strcmp(kind, "binder")) {
    uint32_t maximum = 1;
    Check(gesher::IsBinderFd(observed_target) && ioctl(observed_target, BINDER_SET_MAX_THREADS, &maximum) == 0,
          "actual retained Binder reference completes real broker roundtrip");
  }
  Check(close(source) == -1 && errno == EIO && _get_osfhandle(source) != -1,
        "public close fails closed before changing original ownership");
  // This fresh fixture process owns this precise unpublished fd and performs
  // its explicit teardown outside the production retry path. Quarantine is
  // deliberately not reset; normal process exit closes the original source.
  Check(gesher::CloseUnpublishedDuplicate(observed_target) == 0, "fixture retires its actual identified uncertain alias");
  Check(locks::InspectPrivateReservations().uncertain == 1, "no production quarantine-reset test switch exists");
  Check(!gesher::LookupSocket(observed_target, &socket) && !gesher::LookupEventFd(observed_target) &&
        !gesher::IsBinderFd(observed_target), "fixture teardown retires actual retained virtual references");
  int raw = RawSentinel(sentinel);
  Check(raw >= 0, "raw sentinel publisher opened after quarantine teardown");
  if (raw >= 0) {
    if (raw != observed_target) {
      Check(_dup2(raw, observed_target) == 0, "actual uncertain fd number reused by foreign sentinel");
      Check(_close(raw) == 0, "raw sentinel staging closes after quarantine reuse");
      raw = observed_target;
    }
    Check(locks::DrainPrivateReservations() == EIO && Sentinel(raw), "quarantine never retries/recloses foreign reused fd");
    Check(_close(raw) == 0, "known fixture sentinel closes despite retained quarantine");
  }
}

void RawRace(const char* file, const char* sentinel) {
  int source = open(file, O_RDWR);
  Check(source >= 0, "actual regular source opened for raw publisher race");
  if (source < 0) return;
  Warm(source);
  DWORD before = Handles();
  std::atomic<unsigned> errors{0}, published{0};
  std::thread held([&] {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (reserved_target.load(std::memory_order_acquire) < 0 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    RawOwned owned = RawWitness(sentinel);  // Deliberately bypasses DescriptorGuard.
    if (owned.descriptor < 0 || owned.descriptor == reserved_target.load(std::memory_order_acquire) || !RawCanary(owned)) ++errors;
    raw_ready.store(true, std::memory_order_release);
    while (!raw_release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    if (!raw_release.load(std::memory_order_acquire) || !RawCanary(owned)) ++errors;
    if (!CloseRaw(&owned)) ++errors;
  });
  locks::DuplicateOperations operations{};
  operations.reserved = ActualReservationBarrier;
  int reserved = locks::DuplicateAtLeast(source, 32, false, &operations);
  raw_release.store(true, std::memory_order_release);
  held.join();  // Every barrier above has a finite native deadline; outer CI also bounds the process.
  Check(reserved >= 32 && errors == 0, "actual occupied NUL target survives simultaneous raw CRT sentinel allocation");
  if (reserved >= 0) Check(close(reserved) == 0, "deterministic reservation duplicate closes");

  std::atomic<bool> stop{false}, ready{false};
  std::thread publisher([&] {
    ready.store(true, std::memory_order_release);
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) {
      RawOwned owned = RawWitness(sentinel);
      if (!RawCanary(owned)) ++errors;
      for (unsigned turn = 0; turn < 4; ++turn) std::this_thread::yield();
      if (!RawCanary(owned)) ++errors;
      if (!CloseRaw(&owned)) ++errors;
      ++published;
    }
  });
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) std::this_thread::yield();
  unsigned completed = 0;
  for (; completed < 1000 && std::chrono::steady_clock::now() < end; ++completed) {
    int copy = fcntl(source, F_DUPFD, 8 + completed % 48);
    if (copy < 0) { ++errors; break; }
    bool same = false;
    if (locks::SameDescription(gesher::HandleFromFd(source), gesher::HandleFromFd(copy), &same) || !same) ++errors;
    if (close(copy)) ++errors;
  }
  stop.store(true, std::memory_order_release);
  publisher.join();
  Check(completed == 1000 && published > 0 && errors == 0, "1000 actual minimum duplicates preserve concurrent raw publisher witnesses and sentinel bytes");
  std::printf("ACTUAL_RAW_PUBLISHER duplicates=%u raw-publications=%u errors=%u\n", completed, published.load(), errors.load());
  Check(Empty() && HandleParity(before), "raw publisher race leaves no private fd or native handle growth");
  Check(close(source) == 0, "raw publisher source closes");
}

void Capacity(const char* file, const char* sentinel) {
  int source = open(file, O_RDWR);
  Check(source >= 0, "actual source opened for CRT capacity boundary");
  if (source < 0) return;
  Warm(source);
  DWORD before = Handles();
  std::array<int, 8192> descriptors{};
  size_t count = 0;
  int error = 0;
  for (; count < descriptors.size(); ++count) {
    int fd = RawSentinel(sentinel);  // No emulated capacity or success marker.
    if (fd < 0) { error = gesher_errno_from_crt(errno); break; }
    descriptors[count] = fd;
  }
  Check(count > 0 && count < descriptors.size() && error == EMFILE, "actual CRT allocator reaches real EMFILE");
  unsigned occupied = 0;
  for (int fd = 0; fd < 8192; ++fd) occupied += _get_osfhandle(fd) != -1;
  Check(occupied == 8192, "all8192 actual low-I/O descriptor slots are occupied");
  errno = 123;
  int copy = fcntl(source, F_DUPFD, 32);
  Check(copy == -1 && errno == EMFILE && Empty(), "actual full-table duplicate fails EMFILE with native reservation cleanup");
  Check(_get_osfhandle(source) != -1, "capacity failure preserves original source fd");
  for (size_t index = 0; index < count; ++index) Check(_close(descriptors[index]) == 0, "actual capacity sentinel closes");
  Check(HandleParity(before) && Empty(), "capacity failure returns native handles and private ownership to baseline");
  Check(close(source) == 0, "capacity source closes");
}
}  // namespace

int main(int argc, char** argv) {
  if (!std::getenv("GITHUB_ACTIONS") || std::strcmp(std::getenv("GITHUB_ACTIONS"), "true")) return 90;
  if (argc != 4) return 91;
  _set_invalid_parameter_handler(InvalidParameter);
  if (!std::strcmp(argv[1], "retained-nul")) RetainedNul(argv[2], argv[3]);
  else if (!std::strcmp(argv[1], "raw-race")) RawRace(argv[2], argv[3]);
  else if (!std::strcmp(argv[1], "capacity")) Capacity(argv[2], argv[3]);
  else if (!std::strncmp(argv[1], "quarantine-", 11)) Quarantine(argv[1] + 11, argv[2], argv[3]);
  else FlagRollback(argv[1], argv[2], argv[3]);
  std::printf("ACTUAL_DUPLICATE_UNIT assertions=%u failures=%u scope=actual-resource-dependency-controls atomic-spawn-open\n", assertions, failures);
  return failures ? 1 : 0;
}
