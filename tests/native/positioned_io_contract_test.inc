// Public bionic exports on Windows/ARM64, and the production adapter in the
// host boundary fixture. The ordinary cursor is shared by dup, never by pread.
static void test_positioned_io_contents(void) {
  char path[] = "gesher-positioned-XXXXXX", renamed[128];
  int fd = mkstemp(path);
  check("positioned I/O: create regular fixture", fd >= 0);
  if (fd < 0) return;
  const unsigned char initial[] = {'A', 'B', 'X', 0, 4, 5, 6, 7, 8, 9};
  check("positioned I/O: ordinary initial write", write(fd, initial, sizeof(initial)) == (ssize_t)sizeof(initial));
  check("positioned I/O: initial ordinary cursor", lseek(fd, 7, SEEK_SET) == 7);
  int alias = dup(fd);
  check("positioned I/O: duplicate shares description", alias >= 0);
  unsigned char data[4] = {};
  check("positioned I/O: ABX sniff at explicit offset", pread(fd, data, 4, 0) == 4 && memcmp(data, initial, 4) == 0);
  check("positioned I/O: pread leaves shared cursor", lseek(fd, 0, SEEK_CUR) == 7 && (alias < 0 || lseek(alias, 0, SEEK_CUR) == 7));
  check("positioned I/O: ordinary read still starts at 7", read(fd, data, 1) == 1 && data[0] == 7);
  check("positioned I/O: explicit pwrite changes bytes", pwrite(fd, "yz", 2, 4) == 2);
  check("positioned I/O: pwrite leaves shared cursor", lseek(fd, 0, SEEK_CUR) == 8 && (alias < 0 || lseek(alias, 0, SEEK_CUR) == 8));
  check("positioned I/O: pread64 returns changed bytes", pread64(fd, data, 2, 4) == 2 && memcmp(data, "yz", 2) == 0);
  check("positioned I/O: pwrite64 uses explicit offset", pwrite64(fd, "x", 1, 5) == 1 && pread(fd, data, 2, 4) == 2 && memcmp(data, "yx", 2) == 0);
  check("positioned I/O: aliases preserve shared cursor", lseek(fd, 0, SEEK_CUR) == 8);
  check("positioned I/O: high offset EOF stays 64-bit", pread64(fd, data, 1, (off_t)1 << 33) == 0 && lseek(fd, 0, SEEK_CUR) == 8);
  check("positioned I/O: zero lengths do not move cursor", pread(fd, NULL, 0, 0) == 0 && pwrite(fd, NULL, 0, 1000) == 0 && lseek(fd, 0, SEEK_CUR) == 8);
  struct stat info;
  check("positioned I/O: zero write does not extend file", fstat(fd, &info) == 0 && info.st_size == (off_t)sizeof(initial));
  int ro = open(path, O_RDONLY), wo = open(path, O_WRONLY);
  check("positioned I/O: access witnesses", ro >= 0 && wo >= 0);
  if (ro >= 0) {
    errno = 0;
    check("positioned I/O: readonly pwrite is EBADF", pwrite(ro, "!", 1, 0) == -1 && errno == EBADF);
    close(ro);
  }
  if (wo >= 0) {
    errno = 0;
    check("positioned I/O: writeonly pread is EBADF", pread(wo, data, 1, 0) == -1 && errno == EBADF);
    close(wo);
  }
  errno = 0;
  check("positioned I/O: negative read offset", pread(fd, data, 1, -1) == -1 && errno == EINVAL);
  errno = 0;
  check("positioned I/O: negative write offset", pwrite(fd, data, 1, -1) == -1 && errno == EINVAL);
  errno = 0;
  check("positioned I/O: invalid descriptor", pread(-1, data, 1, 0) == -1 && errno == EBADF);
  snprintf(renamed, sizeof(renamed), "%s-renamed", path);
  check("positioned I/O: rename held file", rename(path, renamed) == 0);
  int replacement = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  check("positioned I/O: replacement witness", replacement >= 0 && write(replacement, "!", 1) == 1);
  check("positioned I/O: renamed fd keeps identity", pread(fd, data, 1, 0) == 1 && data[0] == 'A');
  check("positioned I/O: unlink held file", unlink(renamed) == 0);
  check("positioned I/O: unlinked fd remains readable", pread(fd, data, 1, 0) == 1 && data[0] == 'A');
  check("positioned I/O: unlinked fd remains writable", pwrite(fd, "q", 1, 0) == 1 && pread(fd, data, 1, 0) == 1 && data[0] == 'q');
  check("positioned I/O: unlink preserves ordinary cursor", lseek(fd, 0, SEEK_CUR) == 8);
  if (replacement >= 0) {
    check("positioned I/O: replacement not modified", pread(replacement, data, 1, 0) == 1 && data[0] == '!');
    close(replacement);
  }
  if (alias >= 0) close(alias);
  close(fd);
  unlink(path);
}

static void test_positioned_io_nonseekable(void) {
  unsigned char byte = 0;
  int directory = open(".", O_RDONLY);
  check("positioned I/O: directory witness", directory >= 0);
  if (directory >= 0) {
    errno = 0;
    check("positioned I/O: directory read is EISDIR", pread(directory, &byte, 1, 0) == -1 && errno == EISDIR);
    close(directory);
  }
  int pipes[2];
  int opened = pipe(pipes);
  check("positioned I/O: pipe witness", opened == 0);
  if (opened == 0) {
    check("positioned I/O: pipe initial payload", write(pipes[1], "a", 1) == 1);
    errno = 0;
    check("positioned I/O: pipe read is ESPIPE", pread(pipes[0], &byte, 1, 0) == -1 && errno == ESPIPE);
    errno = 0;
    check("positioned I/O: pipe write is ESPIPE", pwrite(pipes[1], "b", 1, 0) == -1 && errno == ESPIPE);
    check("positioned I/O: pipe failure consumed nothing", read(pipes[0], &byte, 1) == 1 && byte == 'a');
    close(pipes[0]); close(pipes[1]);
  }
  int nullfd = open("/dev/null", O_RDWR), zerofd = open("/dev/zero", O_RDONLY);
  check("positioned I/O: supported device witnesses", nullfd >= 0 && zerofd >= 0);
  if (nullfd >= 0) {
    check("positioned I/O: null device contracts", pread(nullfd, &byte, 1, 123) == 0 && pwrite(nullfd, "x", 1, 123) == 1);
    close(nullfd);
  }
  if (zerofd >= 0) {
    byte = 45;
    check("positioned I/O: zero device reads real zeros", pread(zerofd, &byte, 1, 123) == 1 && byte == 0);
    close(zerofd);
  }
}

// Keep intentional null-buffer calls observable at -O2 and outside fortified
// compile-time diagnostics. These slots still resolve the actual libc imports.
static ssize_t (*volatile positioned_read)(int, void*, size_t, off_t) = pread;
static ssize_t (*volatile positioned_write)(int, const void*, size_t, off_t) = pwrite;

static void test_positioned_io_null_buffers(void) {
  char path[] = "gesher-positioned-null-XXXXXX";
  int fd = mkstemp(path);
  check("positioned null: create empty regular fixture", fd >= 0);
  if (fd < 0) return;
  check("positioned null: ordinary cursor", lseek(fd, 7, SEEK_SET) == 7);
  check("positioned null: empty-file read uses no buffer", positioned_read(fd, NULL, 1, 0) == 0);
  check("positioned null: distant EOF uses no buffer", positioned_read(fd, NULL, 1, 500) == 0);
  errno = 0;
  check("positioned null: regular write needs input", positioned_write(fd, NULL, 1, 0) == -1 && errno == EFAULT);
  check("positioned null: create actual data", pwrite(fd, "x", 1, 0) == 1);
  errno = 0;
  check("positioned null: nonempty read needs output", positioned_read(fd, NULL, 1, 0) == -1 && errno == EFAULT);
  check("positioned null: exact EOF uses no output", positioned_read(fd, NULL, 1, 1) == 0);
  errno = 0;
  check("positioned null: invalid user range still fails at EOF", positioned_read(fd, NULL, SIZE_MAX, 1) == -1 && errno == EFAULT);
  struct stat info;
  check("positioned null: no cursor or file-size change", lseek(fd, 0, SEEK_CUR) == 7 && fstat(fd, &info) == 0 && info.st_size == 1);
  close(fd); unlink(path);

  int directory = open(".", O_RDONLY);
  check("positioned null: directory witness", directory >= 0);
  if (directory >= 0) {
    errno = 0;
    check("positioned null: directory error precedes unused buffer", positioned_read(directory, NULL, 1, 0) == -1 && errno == EISDIR);
    close(directory);
  }
  int pipes[2];
  int opened = pipe(pipes);
  check("positioned null: nonseekable witness", opened == 0);
  if (opened == 0) {
    errno = 0;
    check("positioned null: pipe ESPIPE precedes buffer", positioned_read(pipes[0], NULL, 1, 0) == -1 && errno == ESPIPE);
    errno = 0;
    check("positioned null: pipe ESPIPE precedes access mode", positioned_read(pipes[1], NULL, 1, 0) == -1 && errno == ESPIPE);
    errno = 0;
    check("positioned null: zero-length pipe remains nonseekable", positioned_write(pipes[1], NULL, 0, 0) == -1 && errno == ESPIPE);
    close(pipes[0]); close(pipes[1]);
  }
  int nullfd = open("/dev/null", O_RDWR), zerofd = open("/dev/zero", O_RDWR), randomfd = open("/dev/urandom", O_RDWR);
  check("positioned null: real device witnesses", nullfd >= 0 && zerofd >= 0 && randomfd >= 0);
  const ssize_t max_rw_count = (ssize_t)((size_t)INT32_MAX & ~((size_t)sysconf(_SC_PAGESIZE) - 1));
  if (nullfd >= 0) {
    check("positioned null: NUL read needs no output", positioned_read(nullfd, NULL, 1, 123) == 0);
    check("positioned null: NUL write discards input", positioned_write(nullfd, NULL, 3, 123) == 3);
    check("positioned null: NUL write keeps Linux transfer bound", positioned_write(nullfd, NULL, INT32_MAX, 0) == max_rw_count);
    errno = 0;
    check("positioned null: NUL does not accept invalid user range", positioned_write(nullfd, NULL, SIZE_MAX, 0) == -1 && errno == EFAULT);
    close(nullfd);
  }
  if (zerofd >= 0) {
    errno = 0;
    check("positioned null: zero read still needs output", positioned_read(zerofd, NULL, 1, 0) == -1 && errno == EFAULT);
    check("positioned null: zero write discards input", positioned_write(zerofd, NULL, 3, 123) == 3);
    check("positioned null: zero write keeps Linux transfer bound", positioned_write(zerofd, NULL, INT32_MAX, 0) == max_rw_count);
    close(zerofd);
  }
  if (randomfd >= 0) {
    errno = 0;
    check("positioned null: random read still needs output", positioned_read(randomfd, NULL, 1, 0) == -1 && errno == EFAULT);
    errno = 0;
    check("positioned null: random write still needs entropy input", positioned_write(randomfd, NULL, 1, 0) == -1 && errno == EFAULT);
    close(randomfd);
  }
}

static void test_positioned_io_contracts(void) {
  test_positioned_io_contents();
  test_positioned_io_nonseekable();
  test_positioned_io_null_buffers();
}
