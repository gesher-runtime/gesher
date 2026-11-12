// Public filesystem-space contracts, shared by Linux and both Android guest
// architectures. All paths belong to an ordinary temporary directory. The
// Windows-specific drive/UNC namespace is covered by the host probe/adapter.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int statvfs_same_volume(const struct statvfs* actual, const struct statvfs* expected) {
  return actual->f_bsize == expected->f_bsize && actual->f_frsize == expected->f_frsize &&
         actual->f_blocks == expected->f_blocks && actual->f_namemax == expected->f_namemax;
}

static void statvfs_check_path(const char* path, const struct statvfs* expected, int directory) {
  struct statvfs by_path, by_fd;
  struct statvfs64 by_path64, by_fd64;
  memset(&by_path, 0xa5, sizeof(by_path));
  memset(&by_fd, 0xa5, sizeof(by_fd));
  memset(&by_path64, 0xa5, sizeof(by_path64));
  memset(&by_fd64, 0xa5, sizeof(by_fd64));
  int result = statvfs(path, &by_path);
  check("statvfs accepts an existing directory or regular file", result == 0);
  check("statvfs path reports the containing volume",
        result == 0 && statvfs_same_volume(&by_path, expected));
  result = statvfs64(path, &by_path64);
  check("statvfs64 accepts the same existing path", result == 0);
  check("statvfs64 agrees on block size and capacity",
        result == 0 && by_path64.f_bsize == expected->f_bsize &&
        by_path64.f_frsize == expected->f_frsize && by_path64.f_blocks == expected->f_blocks);

  int fd = open(path, O_RDONLY | (directory ? O_DIRECTORY : 0));
  check("open a read-only descriptor for filesystem statistics", fd >= 0);
  if (fd < 0) return;
  if (!directory) check("position regular file before filesystem query", lseek(fd, 3, SEEK_SET) == 3);
  result = fstatvfs(fd, &by_fd);
  check("fstatvfs accepts the existing file descriptor", result == 0);
  check("fstatvfs descriptor reports the containing volume",
        result == 0 && statvfs_same_volume(&by_fd, expected));
  result = fstatvfs64(fd, &by_fd64);
  check("fstatvfs64 accepts the same descriptor", result == 0);
  check("fstatvfs64 agrees on block size and capacity",
        result == 0 && by_fd64.f_bsize == expected->f_bsize &&
        by_fd64.f_frsize == expected->f_frsize && by_fd64.f_blocks == expected->f_blocks);
  if (!directory) {
    char byte = 0;
    check("filesystem queries preserve the file cursor", lseek(fd, 0, SEEK_CUR) == 3);
    check("filesystem queries retain the open readable descriptor", read(fd, &byte, 1) == 1 && byte == 'd');
  }
  check("close descriptor after filesystem queries", close(fd) == 0);
}

static void test_statvfs_contracts(void) {
  struct statvfs missing;
  errno = 0;
  check("fstatvfs invalid descriptor reports EBADF", fstatvfs(-1, &missing) == -1 && errno == EBADF);
  struct statvfs64 missing64;
  errno = 0;
  check("fstatvfs64 invalid descriptor reports EBADF", fstatvfs64(-1, &missing64) == -1 && errno == EBADF);

  char directory[] = "statvfs-contract-XXXXXX";
  int made = mkdtemp(directory) != NULL;
  check("create filesystem statistics directory", made);
  if (!made) return;
  struct statvfs expected;
  int result = statvfs(directory, &expected);
  check("query filesystem statistics directory", result == 0);
  check("filesystem statistics expose positive units and capacity",
        result == 0 && expected.f_bsize > 0 && expected.f_frsize > 0 && expected.f_blocks > 0);
  if (result != 0) { rmdir(directory); return; }
  statvfs_check_path(directory, &expected, 1);

  char file[1024];
  snprintf(file, sizeof(file), "%s/ordinary-\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.txt", directory);
  int fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
  check("create ordinary Unicode-named file for filesystem queries", fd >= 0);
  if (fd >= 0) {
    check("write ordinary fixture payload", write(fd, "abcdefgh", 8) == 8);
    check("close writer before read-only queries", close(fd) == 0);
    statvfs_check_path(file, &expected, 0);
    check("remove ordinary filesystem statistics file", unlink(file) == 0);
  }

  char absent[1024];
  snprintf(absent, sizeof(absent), "%s/not-created", directory);
  errno = 0;
  check("statvfs missing relative path reports ENOENT", statvfs(absent, &missing) == -1 && errno == ENOENT);
  errno = 0;
  check("statvfs64 missing relative path reports ENOENT", statvfs64(absent, &missing64) == -1 && errno == ENOENT);

  char nested[8][1024];
  const char* parent = directory;
  int levels = 0;
  for (int i = 0; i < 8; ++i) {
    snprintf(nested[i], sizeof(nested[i]), "%s/ordinary-long-directory-%02d-\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", parent, i);
    if (mkdir(nested[i], 0700) != 0) break;
    parent = nested[i];
    ++levels;
  }
  check("create directory path longer than Windows MAX_PATH", levels == 8 && strlen(parent) > 260);
  if (levels == 8) {
    statvfs_check_path(parent, &expected, 1);
    snprintf(file, sizeof(file), "%s/file.txt", parent);
    fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
    check("create ordinary file below the long path", fd >= 0);
    if (fd >= 0) {
      check("write payload below the long path", write(fd, "abcdefgh", 8) == 8);
      check("close long-path writer", close(fd) == 0);
      statvfs_check_path(file, &expected, 0);
      check("remove long-path fixture file", unlink(file) == 0);
    }
  }
  while (levels > 0) check("remove filesystem statistics nested directory", rmdir(nested[--levels]) == 0);
  check("remove filesystem statistics root directory", rmdir(directory) == 0);
}
