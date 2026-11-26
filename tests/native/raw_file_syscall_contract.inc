// Shared by the x86-64 ELF loader and ARM64 guest ABI fixtures.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>

static void test_raw_file_syscalls(void) {
  int fd = (int)syscall(SYS_memfd_create, "raw-file-contract", 0);
  check("file syscalls open a writable memfd", fd >= 0);
  if (fd < 0) return;

  struct stat info;
  errno = EDOM;
  check("raw fallocate grows the file and preserves errno",
        raw6(SYS_fallocate, fd, 0, 0, 4096, 0, 0) == 0 && errno == EDOM &&
            fstat(fd, &info) == 0 && info.st_size == 4096);
  check("named fallocate grows the same file",
        syscall(SYS_fallocate, fd, 0, 0L, 8192L) == 0 &&
            fstat(fd, &info) == 0 && info.st_size == 8192);

  char left[] = "ab", right[] = "xyz";
  struct iovec out[] = {{left, sizeof(left) - 1}, {right, sizeof(right) - 1}};
  char read_left[2] = {0}, read_right[3] = {0};
  struct iovec in[] = {{read_left, sizeof(read_left)}, {read_right, sizeof(read_right)}};
  check("file syscalls set a shared cursor for the positioned I/O test",
        lseek(fd, 7, SEEK_SET) == 7);
  check("raw pwritev writes both buffers at the supplied offset",
        raw6(SYS_pwritev, fd, (long)out, 2, 127, 0, 0) == 5);
  check("named preadv reads the raw write",
        syscall(SYS_preadv, fd, in, 2, 127L, 0L) == 5 &&
            memcmp(read_left, left, 2) == 0 && memcmp(read_right, right, 3) == 0);
  check("raw preadv keeps offsets above 4 GiB without allocating a large file",
        raw6(SYS_preadv, fd, (long)in, 2, 0x10000007fL, 0, 0) == 0);
  left[0] = 'k';
  right[2] = 'q';
  check("named pwritev writes both buffers at the supplied offset",
        syscall(SYS_pwritev, fd, out, 2, 241L, 0L) == 5);
  check("raw preadv reads the named write",
        raw6(SYS_preadv, fd, (long)in, 2, 241, 0, 0) == 5 &&
            memcmp(read_left, left, 2) == 0 && memcmp(read_right, right, 3) == 0);

  errno = EDOM;
  check("raw fadvise succeeds without changing errno",
        raw6(SYS_fadvise64, fd, 127, 5, POSIX_FADV_NORMAL, 0, 0) == 0 && errno == EDOM);
  check("raw fadvise maps a positive backend error and preserves errno",
        raw6(SYS_fadvise64, -1, 0, 0, POSIX_FADV_NORMAL, 0, 0) == -EBADF && errno == EDOM);
  check("named fadvise translates the backend error to -1 and errno",
        syscall(SYS_fadvise64, fd, 0L, 0L, -1) == -1 && errno == EINVAL);
  check("positioned I/O and file advice leave the shared cursor in place",
        lseek(fd, 0, SEEK_CUR) == 7);
  close(fd);
}
