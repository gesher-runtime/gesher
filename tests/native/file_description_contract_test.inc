// Public descriptor contracts run by the native Windows test and the Android
// ELF libc fixture. The same assertions also run against independent Linux.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#endif

static void test_dup3_contracts(void) {
  char source_path[] = "gesher-dup3-source-XXXXXX";
  char target_path[] = "gesher-dup3-target-XXXXXX";
  int source = mkstemp(source_path);
  int target = mkstemp(target_path);
  check("dup3 source and target fixtures open", source >= 0 && target >= 0);
  if (source < 0 || target < 0) goto cleanup;
  check("dup3 target fixture has distinct bytes", write(target, "old", 3) == 3);
  check("dup3 source has independent status flags", fcntl(source, F_SETFL, O_APPEND) == 0);
  check("dup3 target has close-on-exec", fcntl(target, F_SETFD, FD_CLOEXEC) == 0);

  errno = 0;
  check("dup3 rejects unsupported flags", dup3(source, target, O_NONBLOCK) == -1 && errno == EINVAL);
  check("failed dup3 preserves target offset and flags",
        lseek(target, 0, SEEK_CUR) == 3 && fcntl(target, F_GETFD) == FD_CLOEXEC &&
        (fcntl(target, F_GETFL) & O_APPEND) == 0);
  errno = 0;
  check("dup3 rejects self duplication", dup3(target, target, 0) == -1 && errno == EINVAL);
  check("failed self dup3 preserves close-on-exec", fcntl(target, F_GETFD) == FD_CLOEXEC);
  errno = 0;
  check("dup3 rejects invalid source", dup3(-1, target, 0) == -1 && errno == EBADF);
  errno = 0;
  check("dup3 rejects invalid destination", dup3(source, -1, 0) == -1 && errno == EBADF);

  check("dup3 accepts close-on-exec", dup3(source, target, O_CLOEXEC) == target);
  check("dup3 applies close-on-exec only to destination",
        fcntl(target, F_GETFD) == FD_CLOEXEC && fcntl(source, F_GETFD) == 0);
  check("dup3 shares the source status flags", (fcntl(target, F_GETFL) & O_APPEND) != 0);
  check("dup3 alias changes the same open description",
        fcntl(target, F_SETFL, O_NONBLOCK) == 0 &&
        (fcntl(source, F_GETFL) & (O_APPEND | O_NONBLOCK)) == O_NONBLOCK);
  check("dup3 without flags clears close-on-exec",
        dup3(source, target, 0) == target && fcntl(target, F_GETFD) == 0);
cleanup:
  if (source >= 0) close(source);
  if (target >= 0) close(target);
  unlink(source_path);
  unlink(target_path);
}

static void test_stream_description_contracts(void) {
  char source_path[] = "gesher-ofd-stream-XXXXXX";
  char target_path[] = "gesher-ofd-reopen-XXXXXX";
  int source = mkstemp(source_path);
  int target = mkstemp(target_path);
  FILE* stream = NULL;
  int alias = -1;
  check("stream description fixtures open", source >= 0 && target >= 0);
  if (source < 0 || target < 0) goto cleanup;
  close(target);
  target = -1;
  stream = fdopen(source, "w+");
  check("stream description attaches to source", stream != NULL);
  if (!stream) goto cleanup;
  source = -1;
  check("original stream description gets append and nonblocking",
        fcntl(fileno(stream), F_SETFL, O_APPEND | O_NONBLOCK) == 0);
  alias = dup(fileno(stream));
  check("stream original description has a surviving alias", alias >= 0);
  if (alias < 0) goto cleanup;

  stream = freopen(target_path, "r", stream);
  check("freopen opens replacement stream", stream != NULL);
  if (!stream) goto cleanup;
  check("freopen gets new access and status flags",
        (fcntl(fileno(stream), F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) == O_RDONLY);
  check("freopen preserves old description through its dup alias",
        (fcntl(alias, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) ==
            (O_RDWR | O_APPEND | O_NONBLOCK));
  check("replacement flags change independently of surviving old alias",
        fcntl(fileno(stream), F_SETFL, 0) == 0 &&
        (fcntl(alias, F_GETFL) & (O_APPEND | O_NONBLOCK)) == (O_APPEND | O_NONBLOCK));
  target = fileno(stream);
  check("replacement stream closes", fclose(stream) == 0);
  stream = NULL;
  errno = 0;
  check("fclose retires the descriptor", fcntl(target, F_GETFL) == -1 && errno == EBADF);
  target = -1;
  check("fclose leaves original alias usable", fcntl(alias, F_SETFL, O_APPEND) == 0);

cleanup:
  if (stream) fclose(stream);
  if (source >= 0) close(source);
  if (target >= 0) close(target);
  if (alias >= 0) close(alias);
  unlink(source_path);
  unlink(target_path);
}

static void test_file_status_option_contracts(void) {
  char path[] = "gesher-ofd-options-XXXXXX";
  int fd = mkstemp(path);
  check("file status option fixture opens", fd >= 0);
  if (fd < 0) return;
  check("regular files ignore F_SETFL O_ASYNC without a signal-I/O backend",
        fcntl(fd, F_SETFL, O_ASYNC) == 0 && (fcntl(fd, F_GETFL) & O_ASYNC) == 0);
#ifdef _WIN32
  // This backend explicitly has no direct-I/O implementation. Linux permits
  // filesystems to reject it, but a Windows success would be a false promise.
  check("direct-I/O failure fixture has existing flags", fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
  errno = 0;
  check("buffered backend rejects F_SETFL O_DIRECT",
        fcntl(fd, F_SETFL, O_DIRECT | O_APPEND) == -1 && errno == EINVAL);
  check("failed direct-I/O request preserves prior flags",
        (fcntl(fd, F_GETFL) & (O_DIRECT | O_APPEND | O_NONBLOCK)) == O_NONBLOCK);
  errno = 0;
  int direct = open(path, O_RDWR | O_DIRECT);
  check("buffered backend rejects open O_DIRECT", direct == -1 && errno == EINVAL);
  if (direct >= 0) close(direct);
  errno = 0;
  check("unimplemented timestamp suppression fails without changing flags",
        fcntl(fd, F_SETFL, O_NOATIME | O_APPEND) == -1 && errno == EOPNOTSUPP &&
            (fcntl(fd, F_GETFL) & (O_NOATIME | O_APPEND | O_NONBLOCK)) == O_NONBLOCK);
  errno = 0;
  int noatime = open(path, O_RDWR | O_NOATIME);
  check("open does not advertise unimplemented timestamp suppression",
        noatime == -1 && errno == EOPNOTSUPP);
  if (noatime >= 0) close(noatime);
#endif
  close(fd);
  fd = open(path, O_PATH | O_DIRECT | O_APPEND | O_NONBLOCK | O_RDWR);
  check("O_PATH ignores data-access flags", fd >= 0 && fcntl(fd, F_GETFL) == O_PATH);
  if (fd >= 0) {
    errno = 0;
    check("O_PATH rejects F_SETFL", fcntl(fd, F_SETFL, 0) == -1 && errno == EBADF);
    close(fd);
  }
  unlink(path);
}

static void test_vector_append_contracts(void) {
  char path[] = "gesher-ofd-vector-XXXXXX";
  int fd = mkstemp(path);
  check("vector append fixture opens", fd >= 0);
  if (fd < 0) return;
  check("vector append initial bytes and offset", write(fd, "base", 4) == 4 && lseek(fd, 1, SEEK_SET) == 1);
  int alias = dup(fd);
  check("vector append uses a shared description", alias >= 0 && fcntl(alias, F_SETFL, O_APPEND) == 0);
  char a = 'A', b = 'B', c = 'C', d = 'D';
  struct iovec ordinary[] = {{&a, 1}, {NULL, 0}, {&b, 1}};
  struct iovec positioned[] = {{&c, 1}, {&d, 1}};
  check("writev appends its complete vector", writev(fd, ordinary, 3) == 2 && lseek(alias, 0, SEEK_CUR) == 6);
  check("pwritev appends despite its explicit offset", pwritev(alias, positioned, 2, 0) == 2);
  check("positioned vector leaves the shared cursor unchanged", lseek(fd, 0, SEEK_CUR) == 6);
  char actual[8];
  check("scalar and vector appends retain all original bytes", pread(fd, actual, sizeof(actual), 0) == 8 &&
      memcmp(actual, "baseABCD", 8) == 0);
  check("clear append through a vector alias", fcntl(alias, F_SETFL, 0) == 0);
  c = '1'; d = '2';
  check("cleared append restores positioned-vector offsets", pwritev(fd, positioned, 2, 1) == 2 &&
      pread(fd, actual, sizeof(actual), 0) == 8 && memcmp(actual, "b12eABCD", 8) == 0);
  check("positioned vector still retains the ordinary cursor", lseek(alias, 0, SEEK_CUR) == 6);
  check("empty file vector does not change the cursor", writev(fd, NULL, 0) == 0 && lseek(fd, 0, SEEK_CUR) == 6);
  if (alias >= 0) close(alias);
  close(fd); unlink(path);
}

static void test_procfs_description_access(void) {
  int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
  check("procfs descriptor fixture opens", fd >= 0);
  if (fd < 0) return;
  check("procfs records read-only access and descriptor inheritance", (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY &&
      fcntl(fd, F_GETFD) == FD_CLOEXEC);
  char byte;
  check("procfs descriptor supplies real content", read(fd, &byte, 1) == 1);
  errno = 0;
  check("procfs read-only descriptor rejects writes", write(fd, "x", 1) == -1 && errno == EBADF);
#ifdef _WIN32
  check("procfs kernel handle also has no write access", _write(fd, "x", 1) == -1);
#endif
  check("failed procfs write retains the cursor", lseek(fd, 0, SEEK_CUR) == 1);
  close(fd);
  fd = open("/proc/self/comm", O_PATH);
  check("procfs path-only descriptor has no data access", fd >= 0 && fcntl(fd, F_GETFL) == O_PATH);
  if (fd >= 0) {
    errno = 0;
    check("procfs path-only descriptor rejects input", read(fd, &byte, 1) == -1 && errno == EBADF);
    close(fd);
  }
}

static void test_file_description_contracts(void) {
  test_dup3_contracts();
  test_stream_description_contracts();
  test_file_status_option_contracts();
  test_vector_append_contracts();
  test_procfs_description_access();
}
