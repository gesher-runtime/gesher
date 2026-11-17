/* One public-API fixture for Linux libc, the Windows host, and Android ELF
 * x86-64/ARM64 guests. The Windows policy is selected explicitly when building
 * an Android guest; unsupported file/shared discard is an exact expectation,
 * never an alternative accepted in place of a successful Linux operation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#ifndef _WIN32
#include <sys/syscall.h>
#endif
#include <unistd.h>

#if defined(_WIN32) || defined(GESHER_MADVISE_WINDOWS)
#define EXPECT_WINDOWS 1
#else
#define EXPECT_WINDOWS 0
#endif

static int checks, failures;
static size_t page_size;

static void Check(int ok, const char* label) {
  ++checks;
  if (!ok) {
    ++failures;
    fprintf(stderr, "FAIL madvise: %s (errno=%d)\n", label, errno);
  }
}

static int All(const unsigned char* bytes, size_t length, unsigned char value) {
  for (size_t i = 0; i < length; ++i) if (bytes[i] != value) return 0;
  return 1;
}

static int Temporary(char* path, size_t capacity, size_t length) {
  const char* directory = getenv("TEMP");
  if (!directory || !*directory) directory = getenv("TMPDIR");
  if (!directory || !*directory) directory = "/tmp";
  int n = snprintf(path, capacity, "%s/gesher-advice-XXXXXX", directory);
  Check(n > 0 && (size_t)n < capacity, "temporary path fits");
  if (n <= 0 || (size_t)n >= capacity) return -1;
  int fd = mkstemp(path);
  Check(fd >= 0, "create owned backing file");
  if (fd < 0) return -1;
  unsigned char block[4096];
  memset(block, 'B', sizeof(block));
  for (size_t done = 0; done < length;) {
    size_t count = length - done < sizeof(block) ? length - done : sizeof(block);
    if (write(fd, block, count) != (ssize_t)count) {
      Check(0, "write complete backing fixture");
      close(fd); unlink(path); return -1;
    }
    done += count;
  }
  return fd;
}

static void Result(void* address, size_t length, int advice, int expected_error,
                   const char* label) {
  errno = 0;
  int result = madvise(address, length, advice);
  Check(expected_error ? result == -1 && errno == expected_error : result == 0, label);
}

static void Matrix(void) {
  const int kinds[] = {MAP_PRIVATE | MAP_ANONYMOUS, MAP_SHARED | MAP_ANONYMOUS,
                       MAP_PRIVATE, MAP_SHARED};
  const int advice[] = {MADV_DONTNEED, MADV_FREE, MADV_REMOVE};
  for (size_t kind = 0; kind < 4; ++kind) {
    for (size_t operation = 0; operation < 3; ++operation) {
      char path[4096];
      int fd = Temporary(path, sizeof(path), 3 * page_size);
      if (fd < 0) continue;
      const int anonymous = kind < 2;
      const int shared = kind == 1 || kind == 3;
      unsigned char* p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, kinds[kind],
                              anonymous ? -1 : fd, anonymous ? 0 : (off_t)page_size);
      Check(p != MAP_FAILED, "matrix mapping at nonzero file offset");
      if (p == MAP_FAILED) { close(fd); unlink(path); continue; }
      memset(p, 'W', page_size);
      int expected = 0;
      if (advice[operation] == MADV_DONTNEED) {
        if (EXPECT_WINDOWS && kind != 0) expected = EOPNOTSUPP;
      } else if (advice[operation] == MADV_FREE) {
        if (kind != 0) expected = EINVAL;
      } else {
        if (kind == 0) expected = EINVAL;
        else if (kind == 2) expected = EACCES;
        else if (EXPECT_WINDOWS) expected = EOPNOTSUPP;
      }
      char label[128];
      snprintf(label, sizeof(label), "mapping %zu advice %d has exact target result", kind,
               advice[operation]);
      Result(p, page_size, advice[operation], expected, label);
      unsigned char contents = 'W';
      if (!expected) {
        if (advice[operation] == MADV_DONTNEED && !shared)
          contents = anonymous ? 0 : 'B';
        if (advice[operation] == MADV_REMOVE) contents = 0;
      }
      if (!expected && advice[operation] == MADV_FREE) {
        /* Linux may reclaim immediately or retain the old bytes. Windows uses
         * eager discard, which the Linux FREE contract permits. */
        Check(EXPECT_WINDOWS ? All(p, page_size, 0)
                             : All(p, page_size, 0) || All(p, page_size, 'W'),
              "private anonymous FREE uses permitted contents");
      } else {
        Check(All(p, page_size, contents), "matrix mapped contents are preserved or discarded correctly");
      }
      unsigned char stored = 0xff;
      unsigned char expected_file = !anonymous && shared ? contents : 'B';
      Check(pread(fd, &stored, 1, (off_t)page_size) == 1 && stored == expected_file,
            "matrix backing file has exact expected byte");
      Check(munmap(p, page_size) == 0, "release matrix mapping");
      close(fd); unlink(path);
    }
  }
}

static void Admission(void) {
  unsigned char* p = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(p != MAP_FAILED, "admission anonymous mapping");
  if (p == MAP_FAILED) return;
  memset(p, 'W', 3 * page_size);
  Result(p + 1, 0, MADV_DONTNEED, EINVAL, "unaligned address is invalid even at zero length");
  Result(p, 0, 999, EINVAL, "unknown advice is invalid even at zero length");
  Result(p, SIZE_MAX, MADV_DONTNEED, EINVAL, "page rounding overflow is rejected");
  Result((void*)(UINTPTR_MAX - (page_size - 1)), 2 * page_size, MADV_DONTNEED,
         EINVAL, "range end overflow is rejected");
  Result(NULL, 0, MADV_DONTNEED, 0, "valid zero-length advice needs no mapping");
  Result(NULL, page_size, MADV_DONTNEED, ENOMEM, "absent mapping returns ENOMEM");
  Check(All(p, 3 * page_size, 'W'), "invalid advice leaves all owned pages intact");
  Result(p, 1, MADV_DONTNEED, 0, "one-byte request rounds to the complete page");
  Check(All(p, page_size, 0) && All(p + page_size, 2 * page_size, 'W'),
        "page rounding discards only the intersecting page");
  Result(p, 3 * page_size, MADV_DONTFORK, 0, "DONTFORK remains accepted for ART");
  Result(p, 3 * page_size, MADV_DOFORK, 0, "DOFORK remains accepted");
  Check(munmap(p + page_size, page_size) == 0, "create a logical hole inside a reservation");
  p[0] = 'W';
  Result(p, 3 * page_size, MADV_DONTNEED, ENOMEM, "advice crossing a logical hole is rejected");
  if (EXPECT_WINDOWS)
    Check(p[0] == 'W' && p[2 * page_size] == 'W', "full preflight precedes any Windows discard");
  Check(munmap(p, 3 * page_size) == 0, "release the remaining pages around a hole");

  errno = 0;
  Check(mmap(NULL, page_size, PROT_READ, MAP_PRIVATE, -1, 0) == MAP_FAILED && errno == EBADF,
        "missing anonymous flag does not disguise an invalid fd");
  errno = 0;
  Check(mmap(NULL, page_size, PROT_READ, MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL,
        "mapping type is required");
  errno = 0;
  Check(mmap(NULL, SIZE_MAX, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
        errno == ENOMEM, "mmap rounding overflow is rejected");
  p = mmap((void*)(uintptr_t)1, page_size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(p != MAP_FAILED, "nonfixed unaligned address is a valid hint");
  if (p != MAP_FAILED) munmap(p, page_size);
}

static void Protection(void) {
  unsigned char* p = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(p != MAP_FAILED, "PROT_NONE reservation");
  if (p == MAP_FAILED) return;
  Result(p, page_size, MADV_DONTNEED, 0, "live PROT_NONE reservation is not a hole");
  Check(mprotect(p, page_size, PROT_READ | PROT_WRITE) == 0, "commit reserved page");
  Check(All(p, page_size, 0), "reserved page becomes zero-filled");
  memset(p, 'W', page_size);
  Check(mprotect(p, page_size, PROT_READ) == 0, "protect dirty private page read-only");
  Result(p, page_size, MADV_DONTNEED, 0, "read-only private page can be discarded");
  Check(All(p, page_size, 0), "read-only private discard restores zeros");
  Check(mprotect(p, page_size, PROT_READ | PROT_WRITE) == 0, "restore private write permission");
  Check(munmap(p, page_size) == 0, "release protected fixture");
}

static void MemoryLock(void) {
  unsigned char* p = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(p != MAP_FAILED, "memory lock fixture maps three private pages");
  if (p == MAP_FAILED) return;
  memset(p, 'W', 3 * page_size);
  Check(mlock(p + page_size - 1, 2) == 0, "lock two pages intersecting an unaligned range");
  Check(mlock(p, page_size) == 0, "relocking a page does not stack locks");
  Result(p, page_size, MADV_DONTNEED, EINVAL, "DONTNEED rejects the first locked page");
  Result(p + page_size, page_size, MADV_FREE, EINVAL, "FREE rejects the second locked page");
  Check(All(p, 2 * page_size, 'W'), "locked pages retain both pages of data");
  Check(munlock(p + page_size, 2 * page_size) == 0, "unlock a mix of locked and unlocked pages");
  Result(p + page_size, 2 * page_size, MADV_DONTNEED, 0, "discard succeeds across the released range");
  Result(p, page_size, MADV_DONTNEED, EINVAL, "unlock preserves the out-of-range lock");
  Check(munlock(p, page_size) == 0 && munlock(p, page_size) == 0, "repeated unlock is idempotent");
  Result(p, page_size, MADV_DONTNEED, 0, "one unlock releases both lock calls");

  void* last = (void*)(UINTPTR_MAX - (page_size - 1));
  errno = 0;
  Check(mlock(last, 2 * page_size) == -1 && errno == EINVAL, "wrapping lock range is invalid");
  errno = 0;
  Check(munlock(last, 2 * page_size) == -1 && errno == EINVAL, "wrapping unlock range is invalid");
  Check(mlock(p, 0) == 0 && munlock(p, 0) == 0, "aligned zero-length ranges succeed");

#ifndef _WIN32
  /* The Android fixtures target API 26/29. The mlock2 libc export arrived in
   * API 30, but the syscall entry point can be exercised on both guest ABIs. */
  Check(syscall(SYS_mlock2, p, page_size, 0) == 0, "mlock2(0) locks the range");
  Result(p, page_size, MADV_DONTNEED, EINVAL, "mlock2(0) prevents discard");
  Check(syscall(SYS_munlock, p, page_size) == 0, "syscall unlock releases the range");
  Result(p, page_size, MADV_DONTNEED, 0, "syscall unlock makes discard possible");
  errno = 0;
  Check(syscall(SYS_mlock2, p, page_size, 2) == -1 && errno == EINVAL,
        "mlock2 rejects an unknown flag");
  errno = 0;
  Check(mlockall(0) == -1 && errno == EINVAL, "mlockall requires a lock scope");
#ifdef GESHER_MADVISE_WINDOWS
  errno = 0;
  Check(syscall(SYS_mlock2, p, page_size, MLOCK_ONFAULT) == -1 && errno == EOPNOTSUPP,
        "unsupported on-fault locking reports failure");
  errno = 0;
  Check(mlockall(MCL_CURRENT) == -1 && errno == EOPNOTSUPP, "process-wide lock reports failure");
#endif
#endif

  Check(mprotect(p, page_size, PROT_NONE) == 0, "make first page inaccessible");
  errno = 0;
  Check(mlock(p, page_size) == -1 && errno == ENOMEM, "cannot lock an inaccessible page");
  Check(munlock(p, page_size) == 0, "unlock recognizes a mapped inaccessible page");
  Check(munmap(p + page_size, page_size) == 0, "create a hole inside the lock range");
  errno = 0;
  Check(mlock(p + page_size, page_size) == -1 && errno == ENOMEM, "unmapped page cannot be locked");
  errno = 0;
  Check(munlock(p, 3 * page_size) == -1 && errno == ENOMEM, "unlock fails across a logical hole");
  Check(munmap(p, 3 * page_size) == 0, "release memory-lock fixture");
}

static void MemoryLockLifecycle(void) {
  struct rlimit saved;
  const int got_limit = getrlimit(RLIMIT_MEMLOCK, &saved);
  Check(got_limit == 0, "read the memory-lock limit");
  if (got_limit != 0) return;
  struct rlimit limit = saved;
  limit.rlim_cur = page_size;
  if (setrlimit(RLIMIT_MEMLOCK, &limit) != 0) {
    Check(0, "set a one-page soft memory-lock limit");
    return;
  }

  unsigned char* source = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char* target = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(source != MAP_FAILED && target != MAP_FAILED, "map memory-lock lifecycle ranges");
  if (source != MAP_FAILED && target != MAP_FAILED) {
    memset(source, 'L', page_size);
    Check(mlock(source, page_size) == 0, "lock one page with a one-page soft limit");
    Check(mlock(source + 1, 1) == 0, "overlap does not consume an additional lock page");
    errno = 0;
    Check(mlock(source, 2 * page_size) == -1 && errno == ENOMEM,
          "lock growth beyond the soft limit fails with ENOMEM");
    Result(source + page_size, page_size, MADV_DONTNEED, 0,
           "the over-limit page stays unlocked");
    errno = 0;
    Check(mremap(source, page_size, 2 * page_size, 0) == MAP_FAILED && errno == EAGAIN,
          "a locked in-place resize checks its soft limit before growing");
    errno = 0;
    Check(mremap(source, page_size, 2 * page_size, MREMAP_MAYMOVE | MREMAP_FIXED, target)
              == MAP_FAILED && errno == EAGAIN,
          "growing a locked remap beyond its soft limit fails with EAGAIN");
    Check(All(source, page_size, 'L'), "failed locked growth retains the source contents");
    Result(source, page_size, MADV_DONTNEED, EINVAL, "failed locked growth retains the source lock");

    void* moved = mremap(source, page_size, page_size, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    Check(moved == target, "move a locked page at its soft limit");
    if (moved == target) {
      Check(All(target, page_size, 'L'), "locked remap retains the source data");
      Result(target, page_size, MADV_DONTNEED, EINVAL, "moved page remains locked");
      errno = 0;
      Check(mlock(source + page_size, page_size) == -1 && errno == ENOMEM,
            "moved page still consumes one slot in the guest lock budget");
      Check(mmap(target, page_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == target,
            "replace a locked page at its original address");
      Result(target, page_size, MADV_DONTNEED, 0, "replacement mapping has no inherited lock");
    }

    Check(munlockall() == 0 && munlockall() == 0, "process-wide unlock is idempotent");
    Check(mlock(source + page_size, page_size) == 0, "lock tail before partial unmap");
    Check(munmap(source + page_size, page_size) == 0, "unmap the locked tail");
    Check(mlock(target, page_size) == 0, "unmapping a lock refunds the page budget");
    limit.rlim_cur = 0;
    Check(setrlimit(RLIMIT_MEMLOCK, &limit) == 0, "set a zero soft lock limit");
    errno = 0;
    Check(mlock(target, 0) == -1 && errno == EPERM, "zero soft limit denies even a zero-length lock");
    Check(mprotect(target, page_size, PROT_NONE) == 0, "hide a locked page with mprotect");
    Result(target, page_size, MADV_DONTNEED, EINVAL, "mprotect retains the page lock");
    Check(munlockall() == 0, "process-wide unlock succeeds at zero soft limit");
    Check(mprotect(target, page_size, PROT_READ | PROT_WRITE) == 0, "restore access after unlock");
    Result(target, page_size, MADV_DONTNEED, 0, "process-wide unlock releases real pages");
  }
  if (source != MAP_FAILED) munmap(source, 2 * page_size);
  if (target != MAP_FAILED) munmap(target, 2 * page_size);

  limit.rlim_cur = 2 * page_size;
  Check(setrlimit(RLIMIT_MEMLOCK, &limit) == 0, "set a two-page soft memory-lock limit");
  source = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  target = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(source != MAP_FAILED && target != MAP_FAILED, "map locked growth ranges");
  if (source != MAP_FAILED && target != MAP_FAILED) {
    memset(source, 'G', page_size);
    Check(mlock(source, page_size) == 0, "lock the page to grow at the two-page soft limit");
    void* moved = mremap(source, page_size, 2 * page_size, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    Check(moved == target, "grow and move a locked page within its soft limit");
    if (moved == target) {
      Check(All(target, page_size, 'G') && All(target + page_size, page_size, 0),
            "growing a locked mapping preserves data and zero-fills the new page");
      Result(target, page_size, MADV_DONTNEED, EINVAL, "grown mapping keeps its original page lock");
      Result(target + page_size, page_size, MADV_DONTNEED, EINVAL,
             "grown page inherits the original mapping lock");
    }
    Check(munlockall() == 0, "release all growth fixture locks");
  }
  if (source != MAP_FAILED) munmap(source, page_size);
  if (target != MAP_FAILED) munmap(target, 2 * page_size);
  Check(setrlimit(RLIMIT_MEMLOCK, &saved) == 0, "restore the memory-lock limit");
}

static void FileLifetime(void) {
  char path[4096];
  int fd = Temporary(path, sizeof(path), 3 * page_size);
  if (fd < 0) return;
  unsigned char* shared = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                               (off_t)page_size);
  unsigned char* alias = mmap(NULL, 2 * page_size, PROT_READ, MAP_SHARED, fd, (off_t)page_size);
  Check(shared != MAP_FAILED && alias != MAP_FAILED, "independent shared aliases");
  if (shared == MAP_FAILED || alias == MAP_FAILED) {
    if (shared != MAP_FAILED) munmap(shared, 2 * page_size);
    if (alias != MAP_FAILED) munmap(alias, 2 * page_size);
    close(fd); unlink(path); return;
  }
  shared[0] = 'W';
  shared[page_size] = 'N';
  Check(alias[0] == 'W', "independent shared alias observes the write");
  int retained = dup(fd);
  Check(retained >= 0, "retain backing descriptor solely for oracle reads");
  close(fd);
  int replacement = open("/dev/null", O_RDWR);
  Check(replacement >= 0, "open unrelated replacement descriptor");
  if (replacement >= 0 && replacement != fd) {
    Check(dup2(replacement, fd) == fd, "reuse the original fd number");
    close(replacement);
  }
  Result(shared, page_size, MADV_DONTNEED, EXPECT_WINDOWS ? EOPNOTSUPP : 0,
         "shared file advice after original fd close/reuse");
  Check(shared[0] == 'W' && alias[0] == 'W' && shared[page_size] == 'N' && alias[page_size] == 'N',
        "shared file, alias and unadvised neighbor retain their bytes");
  Result(alias, page_size, MADV_WILLNEED, 0, "prefetch a read-only shared view");
  Result(alias, page_size, MADV_DONTFORK, 0, "ART DONTFORK hint accepts a shared file view");
  unsigned char stored = 0;
  if (retained >= 0)
    Check(pread(retained, &stored, 1, (off_t)page_size) == 1 && stored == 'W',
          "closed/reused fd cannot redirect advice to another file");
  if (retained >= 0) close(retained);
  if (replacement >= 0) close(fd);
  munmap(shared, 2 * page_size); munmap(alias, 2 * page_size); unlink(path);
}

static void FileTailAndReplacement(void) {
  char path[4096];
  int fd = Temporary(path, sizeof(path), page_size + 17);
  if (fd < 0) return;
  unsigned char* file = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  Check(file != MAP_FAILED, "private file with a partial final page");
  if (file != MAP_FAILED) {
    Check(file[page_size] == 'B' && file[page_size + 16] == 'B' && file[page_size + 17] == 0,
          "partial file page starts with backing bytes and zero padding");
    file[0] = 'N';
    memset(file + page_size, 'W', page_size);
    Result(file + page_size, page_size, MADV_FREE, EINVAL,
           "copied partial file page is ineligible for anonymous FREE");
    Check(All(file + page_size, page_size, 'W') && file[0] == 'N',
          "rejected file-tail FREE preserves tail and dirty neighbor");
    Result(file + page_size, page_size, MADV_DONTNEED, EXPECT_WINDOWS ? EOPNOTSUPP : 0,
           "private file-tail discard has explicit target result");
    if (EXPECT_WINDOWS)
      Check(All(file + page_size, page_size, 'W'), "unsupported file-tail discard preserves copied bytes");
    else
      Check(file[page_size] == 'B' && file[page_size + 17] == 0,
            "Linux private tail discard restores the backing relationship");
    Check(file[0] == 'N', "unadvised private file page stays dirty");
    munmap(file, 2 * page_size);
  }
  errno = 0;
  Check(mmap(NULL, page_size, PROT_READ, MAP_PRIVATE, fd, 1) == MAP_FAILED && errno == EINVAL,
        "unaligned file offset is rejected");
  unsigned char* mixed = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(mixed != MAP_FAILED, "mixed provenance reservation");
  if (mixed != MAP_FAILED) {
    memset(mixed, 'W', 3 * page_size);
    void* replaced = mmap(mixed + page_size, page_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_FIXED, fd, 0);
    Check(replaced == mixed + page_size, "fixed file pages replace only the middle anonymous page");
    if (replaced == mixed + page_size) {
      mixed[page_size] = 'F';
      Result(mixed, 3 * page_size, MADV_DONTNEED, EXPECT_WINDOWS ? EOPNOTSUPP : 0,
             "mixed anonymous/file range has an exact result");
      Check(EXPECT_WINDOWS ? mixed[0] == 'W' && mixed[page_size] == 'F' && mixed[2 * page_size] == 'W'
                           : mixed[0] == 0 && mixed[page_size] == 'B' && mixed[2 * page_size] == 0,
            "mixed Windows rejection precedes mutation; Linux discards by mapping kind");
      Result(mixed, page_size, MADV_DONTNEED, 0, "anonymous neighbor retains its own provenance");
      replaced = mmap(mixed + page_size, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      Check(replaced == mixed + page_size, "fixed anonymous replacement resets former file origin");
      if (replaced == mixed + page_size) {
        mixed[page_size] = 'R';
        Result(mixed + page_size, page_size, MADV_DONTNEED, 0,
               "formerly file-backed page now permits anonymous discard");
        Check(All(mixed + page_size, page_size, 0), "new anonymous page is zero-filled after discard");
      }
    }
    munmap(mixed, 3 * page_size);
  }
  close(fd); unlink(path);
}

int gesher_selftest(void) {
  checks = failures = 0;
  long page = sysconf(_SC_PAGESIZE);
  Check(page > 0, "page size is available");
  if (page <= 0) return failures;
  page_size = (size_t)page;
  Matrix(); Admission(); Protection(); MemoryLock(); MemoryLockLifecycle();
  FileLifetime(); FileTailAndReplacement();
  printf("madvise contracts (%s): %d checks, %d failures\n",
         EXPECT_WINDOWS ? "Windows policy" : "Linux reference", checks, failures);
  return failures;
}

#ifdef GESHER_MADVISE_MAIN
int main(void) { return gesher_selftest() ? 1 : 0; }
#endif
