// The same public calls run through bionic on Windows and the production
// Windows adapter with a real Linux filesystem as the host test envelope.
#include <sys/socket.h>

static void test_file_advice_contracts(void) {
  char path[] = "gesher-file-advice-XXXXXX";
  int fd = mkstemp(path);
  check("file advice: create regular-file fixture", fd >= 0);
  if (fd < 0) return;
  const char marker[] = "original";
  check("file advice: initialize bytes and offset",
        write(fd, marker, sizeof(marker)) == sizeof(marker) && lseek(fd, 2, SEEK_SET) == 2);
  const int hints[] = {POSIX_FADV_NORMAL, POSIX_FADV_RANDOM, POSIX_FADV_SEQUENTIAL,
                       POSIX_FADV_WILLNEED, POSIX_FADV_DONTNEED, POSIX_FADV_NOREUSE};
  errno = E2BIG;
  for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); ++i) {
    check("file advice: valid hint succeeds without changing errno",
          posix_fadvise(fd, 0, 0, hints[i]) == 0 && errno == E2BIG);
  }
  check("file advice: full-width large range is accepted",
        posix_fadvise(fd, INT64_MAX, INT64_MAX, POSIX_FADV_NORMAL) == 0 && errno == E2BIG);
  check("file advice: Linux accepts a signed offset",
        posix_fadvise(fd, -1, 0, POSIX_FADV_NORMAL) == 0 && errno == E2BIG);
  check("file advice: negative length is EINVAL without changing errno",
        posix_fadvise(fd, 0, -1, POSIX_FADV_NORMAL) == EINVAL && errno == E2BIG);
  check("file advice: invalid hint is EINVAL without changing errno",
        posix_fadvise(fd, 0, 0, -1) == EINVAL && errno == E2BIG &&
        posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE + 1) == EINVAL && errno == E2BIG);
  check("file advice: invalid descriptor takes precedence",
        posix_fadvise(-1, 0, -1, -1) == EBADF && errno == E2BIG);
#if defined(__BIONIC__)
  check("file advice: the 64-bit libc alias uses the same error contract",
        posix_fadvise64(fd, INT64_MAX, -1, POSIX_FADV_NORMAL) == EINVAL && errno == E2BIG);
#endif
  char actual[sizeof(marker)];
  struct stat info;
  check("file advice: shared offset, EOF and contents are preserved",
        lseek(fd, 0, SEEK_CUR) == 2 && fstat(fd, &info) == 0 && info.st_size == sizeof(marker) &&
        pread(fd, actual, sizeof(actual), 0) == sizeof(actual) && memcmp(actual, marker, sizeof(marker)) == 0);

  int path_fd = open(path, O_PATH | O_CLOEXEC);
  check("file advice: open metadata-only descriptor", path_fd >= 0);
  if (path_fd >= 0) {
    errno = E2BIG;
    check("file advice: O_PATH descriptors fail with EBADF before hint validation",
          posix_fadvise(path_fd, 0, -1, -1) == EBADF && errno == E2BIG);
    close(path_fd);
  }
  int pipe_fds[2];
  int status = pipe(pipe_fds);
  check("file advice: create pipe", status == 0);
  if (status == 0) {
    errno = E2BIG;
    check("file advice: pipes fail with ESPIPE before hint validation",
          posix_fadvise(pipe_fds[0], 0, -1, -1) == ESPIPE && errno == E2BIG &&
          posix_fadvise(pipe_fds[1], 0, 0, POSIX_FADV_NORMAL) == ESPIPE && errno == E2BIG);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
  }
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  check("file advice: create socket", socket_fd >= 0);
  if (socket_fd >= 0) {
    errno = E2BIG;
    check("file advice: sockets accept hints and reject invalid input",
          posix_fadvise(socket_fd, 0, 0, POSIX_FADV_NORMAL) == 0 && errno == E2BIG &&
          posix_fadvise(socket_fd, 0, -1, -1) == EINVAL && errno == E2BIG);
    close(socket_fd);
  }
  int null_fd = open("/dev/null", O_RDONLY);
  check("file advice: open character device", null_fd >= 0);
  if (null_fd >= 0) {
    errno = E2BIG;
    check("file advice: a character descriptor follows normal validation",
          posix_fadvise(null_fd, 0, 0, POSIX_FADV_NORMAL) == 0 && errno == E2BIG &&
          posix_fadvise(null_fd, 0, -1, -1) == EINVAL && errno == E2BIG);
    close(null_fd);
  }
  int path_null_fd = open("/dev/null", O_PATH | O_CLOEXEC);
  check("file advice: open metadata-only character descriptor", path_null_fd >= 0);
  if (path_null_fd >= 0) {
    errno = E2BIG;
    check("file advice: O_PATH character descriptors fail before hint validation",
          posix_fadvise(path_null_fd, 0, 0, POSIX_FADV_NORMAL) == EBADF && errno == E2BIG &&
          posix_fadvise(path_null_fd, 0, -1, -1) == EBADF && errno == E2BIG);
    close(path_null_fd);
  }
  check("file advice: close regular descriptor", close(fd) == 0);
  errno = E2BIG;
  check("file advice: closed positive descriptor is EBADF",
        posix_fadvise(fd, 0, 0, POSIX_FADV_NORMAL) == EBADF && errno == E2BIG);
  unlink(path);
}

static void test_file_allocation_contents(void) {
  char path[] = "gesher-file-allocation-XXXXXX";
  int fd = mkstemp(path);
  check("file allocation: create regular fixture", fd >= 0);
  if (fd < 0) return;
  unsigned char marker[16];
  for (unsigned i = 0; i < sizeof(marker); ++i) marker[i] = (unsigned char)(0x41 + i);
  check("file allocation: write existing contents", write(fd, marker, sizeof(marker)) == sizeof(marker));
  check("file allocation: preserve an independent offset", lseek(fd, 5, SEEK_SET) == 5);
  errno = E2BIG;
  int result = posix_fallocate(fd, 0, 65536);
  check("file allocation: grow actual file and preserve errno", result == 0 && errno == E2BIG);
  struct stat info;
  check("file allocation: exact EOF",
        fstat(fd, &info) == 0 && info.st_size == 65536);
  check("file allocation: shared file offset unchanged", lseek(fd, 0, SEEK_CUR) == 5);
  unsigned char actual[16];
  check("file allocation: existing bytes preserved",
        pread(fd, actual, sizeof(actual), 0) == sizeof(actual) && memcmp(actual, marker, sizeof(marker)) == 0);
  unsigned char tail = 255;
  check("file allocation: new tail reads as zero", pread(fd, &tail, 1, 65535) == 1 && tail == 0);
  errno = E2BIG;
  check("file allocation: smaller range succeeds without errno change",
        posix_fallocate(fd, 2, 4) == 0 && errno == E2BIG);
  check("file allocation: smaller range never truncates", fstat(fd, &info) == 0 && info.st_size == 65536);
  int readonly = open(path, O_RDONLY);
  check("file allocation: open read-only witness", readonly >= 0);
  if (readonly >= 0) {
    errno = E2BIG;
    check("file allocation: existing capacity still requires write access",
          posix_fallocate(readonly, 0, 16) == EBADF && errno == E2BIG);
    close(readonly);
  }
  check("file allocation: prepare gap fixture", ftruncate(fd, sizeof(marker)) == 0);
  errno = E2BIG;
  check("file allocation: offset range extends through new EOF",
        posix_fallocate(fd, 4096, 4096) == 0 && errno == E2BIG);
  check("file allocation: offset range grows to its end", fstat(fd, &info) == 0 && info.st_size == 8192);
  tail = 255;
  check("file allocation: gap is zero without overwriting prefix",
        pread(fd, &tail, 1, 3000) == 1 && tail == 0 &&
        pread(fd, actual, sizeof(actual), 0) == sizeof(actual) && memcmp(actual, marker, sizeof(marker)) == 0);
  close(fd);
  unlink(path);
}

static void test_file_allocation_errors(void) {
  char path[] = "gesher-file-allocation-errors-XXXXXX";
  int fd = mkstemp(path);
  check("file allocation: create error fixture", fd >= 0);
  if (fd < 0) return;
  errno = E2BIG;
  check("file allocation: negative offset", posix_fallocate(fd, -1, 1) == EINVAL && errno == E2BIG);
  check("file allocation: negative length", posix_fallocate(fd, 0, -1) == EINVAL && errno == E2BIG);
  check("file allocation: zero length", posix_fallocate(fd, 0, 0) == EINVAL && errno == E2BIG);
  check("file allocation: invalid descriptor keeps errno", posix_fallocate(-1, 0, 1) == EBADF && errno == E2BIG);
  struct stat info;
  check("file allocation: bad ranges leave file empty", fstat(fd, &info) == 0 && info.st_size == 0);
  int pipefd[2];
  int opened = pipe(pipefd);
  check("file allocation: create pipe witness", opened == 0);
  if (opened == 0) {
    errno = E2BIG;
    check("file allocation: pipe is not a disk allocation", posix_fallocate(pipefd[1], 0, 1) == ESPIPE && errno == E2BIG);
    close(pipefd[0]);
    close(pipefd[1]);
  }
  errno = 0;
  check("file allocation: fallocate returns -1 and sets errno", fallocate(fd, 0, -1, 1) == -1 && errno == EINVAL);
  close(fd);
  unlink(path);
}

static void test_file_allocation_overflow(void) {
  char path[] = "gesher-file-allocation-overflow-XXXXXX";
  int fd = mkstemp(path);
  check("file allocation: create overflow fixture", fd >= 0);
  if (fd < 0) return;
  errno = E2BIG;
  check("file allocation: signed end overflow", posix_fallocate(fd, INT64_MAX, 1) == EFBIG && errno == E2BIG);
  check("file allocation: distant end overflow", posix_fallocate(fd, INT64_MAX - 3, 8) == EFBIG && errno == E2BIG);
  struct stat info;
  check("file allocation: overflow leaves file empty", fstat(fd, &info) == 0 && info.st_size == 0);
  close(fd);
  unlink(path);
}

static void test_file_allocation_contracts(void) {
  test_file_advice_contracts();
  test_file_allocation_contents();
  test_file_allocation_errors();
  test_file_allocation_overflow();
}
