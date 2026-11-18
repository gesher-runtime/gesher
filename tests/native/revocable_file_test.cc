// Real Windows/POSIX integration. No substitute descriptor or I/O backend.
#include <gesher/file_description.h>
#include <gesher/revocable_file.h>
#include "../../compat/posix/src/revocable_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

namespace {
unsigned checks;
void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { fprintf(stderr, "FAIL %s (errno=%d, Win32=%lu)\n", reason, errno, GetLastError()); exit(1); }
}
void RejectedIo(int fd) {
  char byte = 'x';
  iovec vector{&byte, 1};
  struct stat info{};
  Check(write(fd, &byte, 1) == -1 && errno == EPERM, "revoked write");
  Check(read(fd, &byte, 1) == -1 && errno == EPERM, "revoked read");
  Check(pwrite(fd, &byte, 1, 0) == -1 && errno == EPERM, "revoked positioned write");
  Check(pread(fd, &byte, 1, 0) == -1 && errno == EPERM, "revoked positioned read");
  Check(writev(fd, &vector, 1) == -1 && errno == EPERM, "revoked vector write");
  Check(readv(fd, &vector, 1) == -1 && errno == EPERM, "revoked vector read");
  Check(fsync(fd) == -1 && errno == EPERM, "revoked sync");
  Check(gesher_ftruncate(fd, 0) == -1 && errno == EPERM, "revoked truncate");
  Check(posix_fallocate(fd, 0, 4096) == EPERM, "revoked allocation");
  Check(fstat(fd, &info) == -1 && errno == EPERM, "revoked size query");
  Check(lseek(fd, 0, SEEK_SET) == -1 && errno == EPERM, "revoked seek");
}
void Lifecycle(bool wait_for_operation) {
  char path[] = "/tmp/gesher-revocable-XXXXXX";
  int fd = mkstemp(path);
  Check(fd >= 0, "create real backing file");
  int trusted = open(path, O_RDWR | O_CLOEXEC);
  Check(trusted >= 0, "independent owner of backing file");
  uint64_t controller = gesher::CreateRevocableFileGroup();
  Check(controller != 0 && gesher::AttachRevocableFileGroup(controller, fd) == 0, "create and attach group");
  Check(gesher::AttachRevocableFileGroup(controller, fd) == -1 && errno == EBUSY, "cannot replace capability");
  Check(write(fd, "payload", 7) == 7, "ordinary write before finalization");
  Check(posix_fallocate(fd, 0, 4096) == 0 && lseek(fd, 0, SEEK_CUR) == 7,
        "allocation extends real file and preserves cursor");
  Check(pwrite(fd, "P", 1, 0) == 1 && lseek(fd, 0, SEEK_CUR) == 7, "positioned write preserves cursor");
  int duplicate = dup(fd);
  Check(duplicate >= 0 && lseek(duplicate, 0, SEEK_CUR) == 7, "dup shares cursor");
  int close_exec = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  Check(close_exec >= 0 && (fcntl(close_exec, F_GETFD) & FD_CLOEXEC) != 0, "fcntl alias has independent descriptor flags");
  const std::string proc = "/proc/self/fd/" + std::to_string(fd);
  int reopened = open(proc.c_str(), O_RDWR);
  Check(reopened >= 0 && lseek(reopened, 0, SEEK_CUR) == 0 && lseek(fd, 0, SEEK_CUR) == 7,
        "proc reopen retains policy with an independent cursor");
  int readonly = open(proc.c_str(), O_RDONLY | O_CLOEXEC);
  Check(readonly >= 0 && write(readonly, "x", 1) == -1 && errno == EBADF, "reopen preserves requested access");
  const std::string readonly_proc = "/proc/self/fd/" + std::to_string(readonly);
  Check(open(readonly_proc.c_str(), O_RDWR) == -1 && errno == EACCES, "read-only capability cannot upgrade rights");
  char link[128]{};
  ssize_t link_size = readlink(proc.c_str(), link, sizeof(link) - 1);
  Check(link_size > 0 && strstr(link, "gesher-revocable") != nullptr && strstr(link, path) == nullptr,
        "proc readlink does not expose private backing pathname");
  Check(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENODEV,
        "shared mappings cannot escape revocation");
  auto* private_view = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
  Check(private_view != MAP_FAILED, "private mappings remain usable");
  private_view[0] = 'X';
  char byte;
  Check(pread(trusted, &byte, 1, 0) == 1 && byte == 'P', "private mapping never changes the backing blob");
  auto snapshot = gesher::CaptureDescriptor(fd);
  Check(gesher::ValidateNativeFileDescription(snapshot) == -1 && errno == EOPNOTSUPP,
        "reject native Windows export without lifecycle support");
  if (wait_for_operation) {
    std::atomic<bool> started{false}, done{false};
    std::thread revoker;
    {
      gesher::FileOperationLease admitted(snapshot);
      Check(admitted.Error() == 0, "admit one in-flight operation");
      revoker = std::thread([&] { started = true; Check(gesher::RevokeFileGroup(controller) == 0, "drained revoke"); done = true; });
      while (!started) Sleep(1);
      Sleep(20);
      Check(!done, "Windows group mutex keeps revoke waiting for admitted operation");
      Check(write(trusted, "owner", 5) == 5, "unrelated owner I/O proceeds during the barrier");
    }
    revoker.join();
    Check(done, "revocation finishes when admitted operation retires");
  } else {
    Check(gesher::RevokeFileGroup(controller) == 0, "revoke group");
  }
  for (int value : {fd, duplicate, close_exec, reopened, readonly}) RejectedIo(value);
  Check(open(proc.c_str(), O_RDWR) == -1 && errno == EPERM, "cannot reopen revoked descriptor");
  gesher::ReleaseRevocableFileGroup(controller);
  Check(munmap(private_view, 4096) == 0, "release private view after revoke");
  for (int value : {fd, duplicate, close_exec, reopened, readonly, trusted}) Check(close(value) == 0, "close aliases");
  snapshot.reset();
  Check(unlink(path) == 0, "release backing file");
}
}
int main() {
  Lifecycle(true);
  DWORD before, after;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before), "sample warm handle count");
  for (unsigned i = 0; i < 32; ++i) Lifecycle(false);
  Check(GetProcessHandleCount(GetCurrentProcess(), &after) && after <= before,
        "repeated sessions release controller, aliases, mutex and sections");
  printf("revocable Windows contracts: %u checks, 0 failures\n", checks);
}
