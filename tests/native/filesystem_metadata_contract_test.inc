/* Public filesystem contracts shared by Linux, the Windows host and both NDK ABIs.
 * Windows CI must supply a different volume via GESHER_METADATA_OTHER_VOLUME.
 * Symlink creation is required: lack of privilege is a failure, never a pass/skip.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static int metadata_checks, metadata_failures;
static void MetadataCheck(int value, const char* name) {
  ++metadata_checks;
  if (!value) {
    ++metadata_failures;
    fprintf(stderr, "FAIL filesystem metadata: %s (errno=%d)\n", name, errno);
  }
}
static void MetadataJoin(char* out, size_t capacity, const char* root, const char* name) {
  int n = snprintf(out, capacity, "%s/%s", root, name);
  MetadataCheck(n >= 0 && (size_t)n < capacity, "bounded fixture path");
}
static int MetadataWrite(const char* path, const char* payload) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) return 0;
  size_t length = strlen(payload);
  int ok = write(fd, payload, length) == (ssize_t)length;
  if (close(fd) != 0) ok = 0;
  return ok;
}
static int MetadataContents(const char* path, const char* expected) {
  char bytes[32];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  ssize_t count = read(fd, bytes, sizeof(bytes));
  close(fd);
  return count == (ssize_t)strlen(expected) && memcmp(bytes, expected, strlen(expected)) == 0;
}
static int MetadataUtimens(int fd, const char* path, const struct timespec* times, int flags) {
#if defined(__linux__) && !defined(__ANDROID__)
  /* Match bionic's syscall route, rather than glibc's null-path rejection. */
  return (int)syscall(SYS_utimensat, fd, path, times, flags);
#else
  typedef int (*TimestampCall)(int, const char*, const struct timespec*, int);
  TimestampCall call = &utimensat;
  return call(fd, path, times, flags);
#endif
}
static int MetadataFutimens(int fd, const struct timespec* times) {
#if defined(__linux__) && !defined(__ANDROID__)
  /* glibc rejects a negative fd before the syscall; bionic does not. */
  return (int)syscall(SYS_utimensat, fd, NULL, times, 0);
#else
  return futimens(fd, times);
#endif
}
static void MetadataTimes(const char* path, int follow, time_t expected, const char* message) {
  struct stat value;
  int status = follow ? stat(path, &value) : lstat(path, &value);
  MetadataCheck(status == 0 && value.st_mtim.tv_sec == expected, message);
}
static void MetadataCrossVolume(const char* root, const char* other) {
  char scratch[4096], from[4096], to[4096];
  MetadataJoin(scratch, sizeof(scratch), other, "gesher-metadata-other-XXXXXX");
  char* created = mkdtemp(scratch);
  MetadataCheck(created != NULL, "create owned cross-volume fixture directory");
  if (!created) return;
  struct stat a, b;
  int distinct = stat(root, &a) == 0 && stat(scratch, &b) == 0 && a.st_dev != b.st_dev;
  MetadataCheck(distinct, "cross-volume fixture really uses a different filesystem");
  if (!distinct) { rmdir(scratch); return; }
  MetadataJoin(from, sizeof(from), root, "cross-source");
  MetadataJoin(to, sizeof(to), scratch, "cross-destination");
  MetadataCheck(MetadataWrite(from, "source") && MetadataWrite(to, "destination"), "cross-volume fixture contents");
  errno = 0;
  MetadataCheck(rename(from, to) == -1 && errno == EXDEV, "rename across volumes returns EXDEV");
  MetadataCheck(MetadataContents(from, "source") && MetadataContents(to, "destination"),
                "cross-volume rename leaves both files intact");
  int oldfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  int newfd = open(scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  MetadataCheck(oldfd >= 0 && newfd >= 0, "cross-volume directory handles");
  errno = 0;
  MetadataCheck(renameat(oldfd, "cross-source", newfd, "cross-destination") == -1 && errno == EXDEV,
                "renameat across volumes returns EXDEV");
  MetadataCheck(MetadataContents(from, "source") && MetadataContents(to, "destination"),
                "cross-volume renameat leaves both files intact");
  char from_dir[4096], to_dir[4096], child[4096];
  MetadataJoin(from_dir, sizeof(from_dir), root, "cross-source-directory");
  MetadataJoin(to_dir, sizeof(to_dir), scratch, "cross-destination-directory");
  MetadataJoin(child, sizeof(child), from_dir, "child");
  MetadataCheck(mkdir(from_dir, 0700) == 0 && mkdir(to_dir, 0700) == 0 && MetadataWrite(child, "retained"),
                "create nonempty source and empty destination directories across volumes");
  struct stat source_directory, target_directory, measured;
  int have_directory_identities = stat(from_dir, &source_directory) == 0 && stat(to_dir, &target_directory) == 0;
  MetadataCheck(have_directory_identities, "retain both directory identities before rename");
  errno = 0;
  MetadataCheck(rename(from_dir, to_dir) == -1 && errno == EXDEV,
                "directory rename across volumes returns EXDEV");
  MetadataCheck(have_directory_identities && stat(to_dir, &measured) == 0 && S_ISDIR(measured.st_mode)
                    && measured.st_dev == target_directory.st_dev && measured.st_ino == target_directory.st_ino,
                "failed cross-volume rename preserves the same empty destination directory");
  MetadataCheck(have_directory_identities && stat(from_dir, &measured) == 0
                    && measured.st_dev == source_directory.st_dev && measured.st_ino == source_directory.st_ino
                    && MetadataContents(child, "retained"), "failed cross-volume rename preserves source directory and child");
  errno = 0;
  MetadataCheck(renameat(oldfd, "cross-source-directory", newfd, "cross-destination-directory") == -1 && errno == EXDEV,
                "directory renameat across volumes returns EXDEV");
  MetadataCheck(have_directory_identities && stat(to_dir, &measured) == 0 && S_ISDIR(measured.st_mode)
                    && measured.st_dev == target_directory.st_dev && measured.st_ino == target_directory.st_ino,
                "failed cross-volume renameat preserves the same empty destination directory");
  MetadataCheck(MetadataContents(child, "retained"), "failed cross-volume renameat preserves source child");
  unlink(child); rmdir(from_dir); rmdir(to_dir);
  if (oldfd >= 0) close(oldfd);
  if (newfd >= 0) close(newfd);
  unlink(from); unlink(to); rmdir(scratch);
  printf("filesystem metadata: cross-volume rename and renameat exercised\n");
}

static void MetadataRelativeLongPath(const char* root) {
  int original = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  MetadataCheck(original >= 0, "retain original current-directory handle");
  if (original < 0) return;
  int moved = chdir(root) == 0;
  MetadataCheck(moved, "enter owned relative-path fixture directory");
  if (moved) {
    char paths[4][4096], file[4096];
    const char* component = "ordinary-relative-directory-abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
    int levels = 0;
    const char* parent = ".";
    for (int i = 0; i < 4; ++i) {
      MetadataJoin(paths[i], sizeof(paths[i]), parent, component);
      int made = mkdir(paths[i], 0700) == 0;
      MetadataCheck(made, "relative mkdir includes current-directory prefix in long-path decision");
      if (!made) break;
      parent = paths[i];
      ++levels;
    }
    MetadataCheck(levels == 4 && strlen(parent) > 260, "create relative directory path beyond MAX_PATH");
    if (levels == 4) {
      struct stat measured;
      MetadataJoin(file, sizeof(file), parent, "relative-קובץ.txt");
      MetadataCheck(MetadataWrite(file, "relative") && chmod(file, 0400) == 0 && stat(file, &measured) == 0 &&
                    !(measured.st_mode & 0200), "relative long Unicode file uses shared host chmod");
      chmod(file, 0600); unlink(file);
    }
    while (levels > 0) MetadataCheck(rmdir(paths[--levels]) == 0, "remove owned long relative directory");
    MetadataCheck(fchdir(original) == 0, "restore original directory through its fd");
  }
  close(original);
}

static int filesystem_metadata_contracts(void) {
  metadata_checks = metadata_failures = 0;
  const char* temporary = getenv("TEMP");
  if (!temporary || !*temporary) temporary = getenv("TMPDIR");
  if (!temporary || !*temporary) temporary = "/tmp";
  char root[4096], file[4096], link[4096], dangling[4096], directory[4096], dirlink[4096], missing[4096];
  MetadataJoin(root, sizeof(root), temporary, "gesher-metadata-XXXXXX");
  MetadataCheck(mkdtemp(root) != NULL, "create isolated fixture directory");
  if (metadata_failures) return metadata_failures;
  MetadataJoin(file, sizeof(file), root, "קובץ.txt");
  MetadataJoin(link, sizeof(link), root, "link");
  MetadataJoin(dangling, sizeof(dangling), root, "dangling");
  MetadataJoin(directory, sizeof(directory), root, "directory");
  MetadataJoin(dirlink, sizeof(dirlink), root, "directory-link");
  MetadataJoin(missing, sizeof(missing), root, "missing");
  MetadataCheck(MetadataWrite(file, "payload"), "create Unicode file");
  MetadataCheck(mkdir(directory, 0700) == 0, "create directory");
  MetadataCheck(symlink(file, link) == 0, "create real file symlink (required)");
  MetadataCheck(symlink(missing, dangling) == 0, "create real dangling symlink (required)");
  MetadataCheck(symlink(directory, dirlink) == 0, "create real directory symlink (required)");
  struct timespec first[2] = {{1700000000, 0}, {1700000001, 0}};
  struct timespec second[2] = {{1700000100, 0}, {1700000101, 0}};
  struct timespec omitted[2] = {{INT64_MAX, UTIME_OMIT}, {INT64_MIN, UTIME_OMIT}};
  struct timespec invalid[2] = {{0, -1}, {0, 0}};
  MetadataCheck(utimensat(AT_FDCWD, file, first, 0) == 0, "set target timestamps");
  MetadataCheck(utimensat(AT_FDCWD, link, second, AT_SYMLINK_NOFOLLOW) == 0, "set link timestamps without following");
  MetadataTimes(file, 1, first[1].tv_sec, "nofollow leaves target time intact");
  MetadataTimes(link, 0, second[1].tv_sec, "nofollow changes actual link time");
  MetadataCheck(utimensat(AT_FDCWD, link, second, 0) == 0, "set timestamps through link");
  MetadataTimes(file, 1, second[1].tv_sec, "follow changes target time");
  MetadataCheck(utimensat(AT_FDCWD, dangling, first, AT_SYMLINK_NOFOLLOW) == 0, "timestamp dangling link itself");
  MetadataTimes(dangling, 0, first[1].tv_sec, "dangling link has requested timestamp");
  errno = 0;
  MetadataCheck(utimensat(AT_FDCWD, dangling, first, 0) == -1 && errno == ENOENT, "following dangling link fails");
  MetadataCheck(utimensat(AT_FDCWD, dirlink, second, AT_SYMLINK_NOFOLLOW) == 0, "timestamp directory symlink itself");
  MetadataTimes(dirlink, 0, second[1].tv_sec, "directory symlink metadata changed");
  struct timeval micros[2] = {{1700000200, 123400}, {1700000201, 567800}};
  MetadataCheck(lutimes(link, micros) == 0, "lutimes nofollow");
  MetadataTimes(link, 0, micros[1].tv_sec, "lutimes changes link");
  MetadataTimes(file, 1, second[1].tv_sec, "lutimes leaves target intact");
  MetadataCheck(utimes(file, micros) == 0, "utimes updates actual file");
  struct stat measured;
  MetadataCheck(stat(file, &measured) == 0 && measured.st_mtim.tv_sec == micros[1].tv_sec &&
                measured.st_mtim.tv_nsec == micros[1].tv_usec * 1000L, "microsecond precision preserved on fixture filesystem");
  MetadataCheck(MetadataUtimens(-1, missing, omitted, 0x40000000) == 0, "both OMIT bypass path/fd/flags");
  MetadataCheck(MetadataUtimens(AT_FDCWD, NULL, omitted, 0) == 0, "both OMIT bypass null path");
  MetadataCheck(MetadataFutimens(-1, omitted) == 0, "futimens both OMIT bypass invalid fd");
  errno = 0;
  MetadataCheck(utimensat(AT_FDCWD, missing, invalid, 0) == -1 && errno == ENOENT, "missing path precedes bad nanoseconds");
  errno = 0;
  MetadataCheck(utimensat(AT_FDCWD, file, invalid, 0) == -1 && errno == EINVAL, "existing path rejects bad nanoseconds");
  errno = 0;
  MetadataCheck(utimensat(AT_FDCWD, missing, first, 0x4000) == -1 && errno == EINVAL, "invalid flags precede missing path");
  errno = 0;
  MetadataCheck(MetadataUtimens(AT_FDCWD, NULL, first, 0) == -1 && errno == EFAULT, "null path with AT_FDCWD");
  int fd = open(file, O_RDONLY | O_CLOEXEC);
  int dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  int childfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  MetadataCheck(fd >= 0 && dirfd >= 0 && childfd >= 0, "open read-only file and directory handles");
  MetadataCheck(MetadataFutimens(fd, first) == 0, "futimens through read-only fd");
  MetadataTimes(file, 1, first[1].tv_sec, "futimens updates original object");
  MetadataCheck(MetadataFutimens(childfd, first) == 0, "futimens through directory handle");
  MetadataTimes(directory, 1, first[1].tv_sec, "directory handle timestamps");
  errno = 0;
  MetadataCheck(MetadataUtimens(fd, NULL, first, AT_SYMLINK_NOFOLLOW) == -1 && errno == EINVAL, "fd null-path route rejects flags");
  MetadataCheck(MetadataUtimens(fd, NULL, second, 0) == 0, "fd null-path route succeeds");
  MetadataCheck(futimes(fd, micros) == 0, "futimes through read-only descriptor");
  MetadataTimes(file, 1, micros[1].tv_sec, "futimes applies converted microseconds");
#if defined(_WIN32) || defined(__ANDROID__)
  /* bionic validates timeval before lookup; glibc's wrapper differs here. */
  {
    struct timeval bad_micros[2] = {{0, 1000000}, {0, 0}};
    errno = 0;
    MetadataCheck(lutimes(missing, bad_micros) == -1 && errno == EINVAL,
                  "invalid timeval precedes missing link lookup");
    errno = 0;
    MetadataCheck(futimes(-1, bad_micros) == -1 && errno == EINVAL,
                  "invalid timeval precedes invalid fd lookup");
  }
#endif
  errno = 0;
  MetadataCheck(utimensat(dirfd, "", first, 0) == -1 && errno == ENOENT, "empty directory path requires AT_EMPTY_PATH");
  MetadataCheck(utimensat(fd, "", first, AT_EMPTY_PATH) == 0, "AT_EMPTY_PATH updates ordinary fd");
  MetadataTimes(file, 1, first[1].tv_sec, "empty-path ordinary fd time");
  MetadataCheck(utimensat(childfd, "", second, AT_EMPTY_PATH) == 0, "AT_EMPTY_PATH updates directory fd");
  MetadataCheck(utimensat(dirfd, "קובץ.txt", second, 0) == 0, "directory-relative Unicode timestamp");
  errno = 0;
  MetadataCheck(utimensat(fd, "child", first, 0) == -1 && errno == ENOTDIR, "ordinary fd cannot resolve relative child");
  int pipefd[2];
  int pipe_ok = pipe(pipefd) == 0;
  MetadataCheck(pipe_ok, "create non-disk fixture descriptor");
  if (pipe_ok) {
    errno = 0;
    MetadataCheck(MetadataFutimens(pipefd[0], invalid) == -1 && errno == EINVAL, "valid non-disk fd validates nanoseconds");
    close(pipefd[0]); close(pipefd[1]);
  }
#if !defined(_WIN32)
  {
    const int64_t bad[] = {INT64_C(1) << 32, (INT64_C(1) << 32) + UTIME_NOW,
                           (INT64_C(1) << 32) + UTIME_OMIT};
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      struct timespec wide[2] = {{0, (long)bad[i]}, {0, UTIME_OMIT}};
      errno = 0;
      MetadataCheck(utimensat(AT_FDCWD, file, wide, 0) == -1 && errno == EINVAL,
                    "LP64 guest full-width invalid nanoseconds rejected");
      errno = 0;
      MetadataCheck(utimensat(AT_FDCWD, missing, wide, 0) == -1 && errno == ENOENT,
                    "LP64 guest nanoseconds retain path lookup order");
    }
  }
#endif
  struct timespec leave_access[2] = {{INT64_MIN, UTIME_OMIT}, {1700000300, 0}};
  struct stat before;
  MetadataCheck(stat(file, &before) == 0 && MetadataFutimens(fd, leave_access) == 0 && fstat(fd, &measured) == 0 &&
                measured.st_atim.tv_sec == before.st_atim.tv_sec && measured.st_mtim.tv_sec == 1700000300,
                "single OMIT preserves access time and original descriptor");
  MetadataCheck(MetadataFutimens(fd, NULL) == 0 && fstat(fd, &measured) == 0 && measured.st_mtim.tv_sec > 1700000300,
                "null times updates to current time");
  MetadataCheck(chmod(file, 0400) == 0 && stat(file, &measured) == 0 && !(measured.st_mode & 0200),
                "Unicode chmod clears owner write bit");
  MetadataCheck(fchmodat(dirfd, "קובץ.txt", 0600, 0) == 0 && stat(file, &measured) == 0 && (measured.st_mode & 0200),
                "directory-relative fchmodat restores write bit");
  errno = 0;
  MetadataCheck(fchmodat(dirfd, "missing", 0600, 0x4000) == -1 && errno == EINVAL, "fchmodat retains invalid flags");
  errno = 0;
  MetadataCheck(fchmodat(AT_FDCWD, link, 0400, AT_SYMLINK_NOFOLLOW) == -1 && errno == EOPNOTSUPP,
                "nofollow chmod on symlink reports unsupported mode change");
  MetadataCheck(stat(file, &measured) == 0 && (measured.st_mode & 0200), "nofollow chmod cannot change target attributes");
  MetadataCheck(remove(dirlink) == 0 && stat(directory, &measured) == 0 && S_ISDIR(measured.st_mode),
                "remove directory symlink preserves target directory");
  errno = 0;
  MetadataCheck(lstat(dirlink, &measured) == -1 && errno == ENOENT, "directory symlink name removed");
  if (fd >= 0) { MetadataCheck(fstat(fd, &measured) == 0, "timestamp calls preserve borrowed fd"); close(fd); }
  if (dirfd >= 0) close(dirfd);
  if (childfd >= 0) close(childfd);

  /* A >260-character path exercises the shared UTF-8/long-path host mapping. */
  char deep[3][4096], longfile[4096];
  const char* component = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv";
  size_t depth;
  for (depth = 0; depth < 3; ++depth) {
    MetadataJoin(deep[depth], sizeof(deep[depth]), depth ? deep[depth - 1] : root, component);
    MetadataCheck(mkdir(deep[depth], 0700) == 0, "create long-path fixture component");
  }
  MetadataJoin(longfile, sizeof(longfile), deep[2], "ארוך.txt");
  MetadataCheck(strlen(longfile) > 260 && MetadataWrite(longfile, "long"), "create long Unicode file");
  MetadataCheck(chmod(longfile, 0400) == 0 && stat(longfile, &measured) == 0 && !(measured.st_mode & 0200),
                "long-path chmod uses host path mapper");
  MetadataCheck(chmod(longfile, 0600) == 0 && utimensat(AT_FDCWD, longfile, first, 0) == 0,
                "long-path write mode and timestamps");
  unlink(longfile);
  for (depth = 3; depth > 0; --depth) rmdir(deep[depth - 1]);
  MetadataRelativeLongPath(root);
  if (((root[0] >= 'a' && root[0] <= 'z') || (root[0] >= 'A' && root[0] <= 'Z')) && root[1] == ':') {
    char mapped[4104];
    int n = snprintf(mapped, sizeof(mapped), "/mnt/%c%s/קובץ.txt", root[0], root + 2);
    MetadataCheck(n > 0 && (size_t)n < sizeof(mapped), "bounded mapped-drive alias");
    size_t i;
    for (i = 0; mapped[i]; ++i) if (mapped[i] == '\\') mapped[i] = '/';
    MetadataCheck(chmod(mapped, 0400) == 0 && stat(file, &measured) == 0 && !(measured.st_mode & 0200),
                  "/mnt drive alias chmod updates actual file");
    MetadataCheck(chmod(mapped, 0600) == 0 && utimensat(AT_FDCWD, mapped, second, 0) == 0,
                  "/mnt drive alias timestamps and write mode");
    printf("filesystem metadata: mapped Windows drive path exercised\n");
  } else {
#if defined(_WIN32) || defined(__ANDROID__)
    MetadataCheck(0, "Windows fixture TEMP must identify its drive for /mnt assertions");
#endif
  }
  char from[4096], to[4096];
  MetadataJoin(from, sizeof(from), root, "rename-source");
  MetadataJoin(to, sizeof(to), root, "rename-target");
  MetadataCheck(MetadataWrite(from, "new") && MetadataWrite(to, "old"), "same-volume rename fixture");
  int held = open(to, O_RDONLY | O_CLOEXEC);
  MetadataCheck(held >= 0 && rename(from, to) == 0 && MetadataContents(to, "new"),
                "rename replaces an open target on same volume");
  char old_bytes[4] = {0};
  MetadataCheck(held >= 0 && read(held, old_bytes, 3) == 3 && memcmp(old_bytes, "old", 3) == 0,
                "open replaced file retains its original data");
  if (held >= 0) close(held);
  unlink(from); unlink(to);
  const char* other = getenv("GESHER_METADATA_OTHER_VOLUME");
#if defined(__linux__) && !defined(__ANDROID__)
  if (!other || !*other) other = "/dev/shm";
#endif
  if (other && *other) MetadataCrossVolume(root, other);
  else MetadataCheck(0, "cross-volume fixture path is required");
  unlink(link); unlink(dangling); unlink(dirlink); unlink(file); rmdir(directory); rmdir(root);
  printf("filesystem metadata contracts: %d checks, %d failures\n", metadata_checks, metadata_failures);
  return metadata_failures;
}
