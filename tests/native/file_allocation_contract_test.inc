// The same public calls run through bionic on Windows and the production
// Windows adapter with a real Linux filesystem as the host test envelope.
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
  test_file_allocation_contents();
  test_file_allocation_errors();
  test_file_allocation_overflow();
}
