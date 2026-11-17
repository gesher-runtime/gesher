/* Actual public-provider and legacy syscall contracts. Linux/glibc is a raw
 * reference only: Android Bionic alone adds the three O_PATH fallbacks. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#if defined(_WIN32)
#include <gesher/xattr.h>
#define XGET gesher::BionicFgetxattr
#define XSET gesher::BionicFsetxattr
#define XLIST gesher::BionicFlistxattr
#define XRAW(name, ...) name(__VA_ARGS__)
#else
#include <sys/syscall.h>
#define XGET fgetxattr
#define XSET fsetxattr
#define XLIST flistxattr
#define XRAW(name, ...) syscall(SYS_##name, __VA_ARGS__)
#endif
#ifndef O_PATH
#define O_PATH 0x200000
#endif

static int xattr_checks, xattr_failures;
static void XCheck(int value, const char* name) {
  ++xattr_checks;
  if (!value) {
    ++xattr_failures;
    fprintf(stderr, "FAIL xattr fd: %s (errno=%d)\n", name, errno);
  }
}
#define XERROR(call, expected, message) do { errno = 0; \
  long xresult = (long)(call); int xerror = errno; \
  XCheck(xresult == -1 && xerror == (expected), message); } while (0)

static void XattrBadPointers(const char* path, int fd) {
  volatile uintptr_t invalid = 1;
  char* bad = (char*)invalid;
  char output[256];
  XERROR(getxattr(path, bad, output, sizeof(output)), EFAULT, "bad public name pointer");
  XERROR(getxattr(bad, "user.base", output, sizeof(output)), EFAULT, "bad public path pointer");
  XERROR(setxattr(path, "user.base", bad, 4, 0), EFAULT, "bad public input pointer");
  XERROR(XGET(fd, "user.base", bad, 4), EFAULT, "bad public output pointer");
  XERROR(XRAW(fgetxattr, fd, bad, output, sizeof(output)), EFAULT, "bad raw name pointer");
  XERROR(XRAW(fsetxattr, fd, "user.base", bad, 4, 0), EFAULT, "bad raw input pointer");
  XERROR(XRAW(fgetxattr, fd, "user.base", bad, 4), EFAULT, "bad raw output pointer");
  XCheck(XGET(fd, "user.base", bad, 0) == 4, "size query never touches the output pointer");
  XCheck(XGET(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "failed pointer copies preserve the stored value");

  long page = sysconf(_SC_PAGESIZE);
  XCheck(page > 0, "discover real page size");
  if (page <= 0) return;
  char* pages = (char*)mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  XCheck(pages != MAP_FAILED, "allocate pointer boundary fixture");
  if (pages == MAP_FAILED) return;
  memcpy(pages + page - 10, "user.base", 10);
  XCheck(mprotect(pages + page, (size_t)page, PROT_NONE) == 0, "guard the following page");
  XCheck(XGET(fd, pages + page - 10, output, sizeof(output)) == 4,
         "name terminator before a guard page is sufficient");
  XERROR(XGET(fd, pages + page, output, sizeof(output)), EFAULT, "unreadable name page");
  XERROR(XSET(fd, "user.base", pages + page, 4, 0), EFAULT, "unreadable input page");
  memset(pages, 0x5a, (size_t)page - 10);
  XCheck(mprotect(pages, (size_t)page, PROT_READ) == 0, "make output page readonly");
  XERROR(XGET(fd, "user.base", pages, 4), EFAULT, "readonly output must not become writable");
  XERROR(XLIST(fd, pages, (size_t)page), EFAULT, "readonly list output");
  XCheck((unsigned char)pages[0] == 0x5a, "readonly output remains unchanged");
  XCheck(munmap(pages, (size_t)page * 2) == 0, "release pointer boundary fixture");
}

static void XattrRawRoutes(const char* path, int fd) {
  char output[256];
  XCheck(XRAW(setxattr, path, "user.raw-path", "p", 1, XATTR_CREATE) == 0,
         "raw setxattr number");
  XCheck(XRAW(lsetxattr, path, "user.raw-link", "l", 1, XATTR_CREATE) == 0,
         "raw lsetxattr number");
  XCheck(XRAW(fsetxattr, fd, "user.raw-fd", "f", 1, XATTR_CREATE) == 0,
         "raw fsetxattr number");
  XCheck(XRAW(getxattr, path, "user.raw-fd", output, sizeof(output)) == 1 && output[0] == 'f',
         "raw getxattr reads the same object");
  XCheck(XRAW(lgetxattr, path, "user.raw-path", output, sizeof(output)) == 1 && output[0] == 'p',
         "raw lgetxattr reads the same object");
  XCheck(XRAW(fgetxattr, fd, "user.raw-link", output, sizeof(output)) == 1 && output[0] == 'l',
         "raw fgetxattr reads the same object");
  XCheck(XRAW(listxattr, path, output, sizeof(output)) > 0, "raw listxattr number");
  XCheck(XRAW(llistxattr, path, output, sizeof(output)) > 0, "raw llistxattr number");
  XCheck(XRAW(flistxattr, fd, output, sizeof(output)) > 0, "raw flistxattr number");
  XCheck(XRAW(removexattr, path, "user.raw-fd") == 0, "raw removexattr number");
  XCheck(XRAW(lremovexattr, path, "user.raw-path") == 0, "raw lremovexattr number");
  XCheck(XRAW(fremovexattr, fd, "user.raw-link") == 0, "raw fremovexattr number");
}

static void XattrPathFd(int pathfd) {
  char output[256];
#if defined(__ANDROID__) || defined(_WIN32)
  XCheck(XGET(pathfd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "Bionic public fgetxattr supports O_PATH");
  XCheck(XSET(pathfd, "user.public", "public", 6, 0) == 0,
         "Bionic public fsetxattr supports O_PATH");
  XCheck(XLIST(pathfd, output, sizeof(output)) > 0, "Bionic public flistxattr supports O_PATH");
#else
  XERROR(XGET(pathfd, "user.base", output, sizeof(output)), EBADF, "glibc fget has no Bionic fallback");
  XERROR(XSET(pathfd, "user.public", "public", 6, 0), EBADF, "glibc fset has no Bionic fallback");
  XERROR(XLIST(pathfd, output, sizeof(output)), EBADF, "glibc flist has no Bionic fallback");
#endif
  XERROR(fremovexattr(pathfd, "user.base"), EBADF, "public remove has no O_PATH fallback");
  XERROR(XRAW(fgetxattr, pathfd, "user.base", output, sizeof(output)), EBADF, "raw get rejects O_PATH");
  XERROR(XRAW(fsetxattr, pathfd, "user.raw", "raw", 3, 0), EBADF, "raw set rejects O_PATH");
  XERROR(XRAW(flistxattr, pathfd, output, sizeof(output)), EBADF, "raw list rejects O_PATH");
  XERROR(XRAW(fremovexattr, pathfd, "user.base"), EBADF, "raw remove rejects O_PATH");
}

static int xattr_fd_contracts(void) {
  xattr_checks = xattr_failures = 0;
  const char* temporary = getenv("TEMP");
  if (!temporary || !*temporary) temporary = getenv("TMPDIR");
  if (!temporary || !*temporary) temporary = "/tmp";
  char root[4096], path[4096], moved[4096], symbolic[4096];
  int length = snprintf(root, sizeof(root), "%s/gesher-xattr-fd-XXXXXX", temporary);
  XCheck(length > 0 && (size_t)length < sizeof(root) && mkdtemp(root) != NULL, "create private fixture directory");
  if (xattr_failures) return xattr_failures;
  XCheck(snprintf(path, sizeof(path), "%s/file", root) < (int)sizeof(path) &&
         snprintf(moved, sizeof(moved), "%s/moved", root) < (int)sizeof(moved) &&
         snprintf(symbolic, sizeof(symbolic), "%s/link", root) < (int)sizeof(symbolic), "bounded paths");
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  XCheck(fd >= 0 && fsetxattr(fd, "user.base", "base", 4, 0) == 0, "create real attribute fixture");
  if (fd < 0) { rmdir(root); return xattr_failures; }
  XattrBadPointers(path, fd);
  XattrRawRoutes(path, fd);
  int pathfd = open(path, O_PATH | O_CLOEXEC);
  XCheck(pathfd >= 0, "open path-only descriptor");
  if (pathfd >= 0) XattrPathFd(pathfd);

  XCheck(symlink(path, symbolic) == 0, "create real symlink fixture");
  int linkfd = open(symbolic, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  XCheck(linkfd >= 0, "open the symlink itself with O_PATH");
#if defined(__ANDROID__) || defined(_WIN32)
  if (linkfd >= 0) {
    char out[256];
    XERROR(XGET(linkfd, "user.base", out, sizeof(out)), ENODATA, "O_PATH symlink fallback never reads the target");
    XERROR(XSET(linkfd, "user.base", "other", 5, 0), EPERM, "O_PATH symlink fallback never writes the target");
    XCheck(XLIST(linkfd, out, sizeof(out)) >= 0, "O_PATH symlink list is permitted");
    XERROR(fremovexattr(linkfd, "user.base"), EBADF, "public O_PATH symlink remove stays raw");
  }
#endif
  if (linkfd >= 0) close(linkfd);
  unlink(symbolic);

  XCheck(rename(path, moved) == 0, "rename while both descriptors remain open");
  int replacement = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  XCheck(replacement >= 0 && fsetxattr(replacement, "user.base", "new", 3, 0) == 0,
         "replace the original pathname with another inode");
  char output[256];
  XCheck(XGET(fd, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "admitted fd retains the old inode after pathname replacement");
  if (pathfd >= 0) XattrPathFd(pathfd);
  int held = dup(fd);
  XCheck(held >= 0 && dup2(replacement, fd) == fd, "reuse the original fd number");
  XCheck(XGET(fd, "user.base", output, sizeof(output)) == 3 && !memcmp(output, "new", 3),
         "reused fd resolves to its new object");
  XCheck(XGET(held, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "duplicate retains its original object independently");
  XCheck(unlink(moved) == 0, "unlink the final pathname of the held inode");
  XCheck(XGET(held, "user.base", output, sizeof(output)) == 4 && !memcmp(output, "base", 4),
         "get on a held unlinked inode");
  XCheck(XSET(held, "user.unlinked", "u", 1, 0) == 0, "set on a held unlinked inode");
  XCheck(XLIST(held, output, sizeof(output)) > 0, "list on a held unlinked inode");
  XCheck(fremovexattr(held, "user.unlinked") == 0, "remove on a held unlinked inode");
#if defined(__ANDROID__) || defined(_WIN32)
  if (pathfd >= 0) {
    XCheck(XGET(pathfd, "user.base", output, sizeof(output)) == 4,
           "Bionic O_PATH get retains an unlinked inode");
    XCheck(XSET(pathfd, "user.after-unlink", "u", 1, 0) == 0,
           "Bionic O_PATH set retains an unlinked inode");
    XCheck(XLIST(pathfd, output, sizeof(output)) > 0, "Bionic O_PATH list retains an unlinked inode");
  }
#endif
  if (held >= 0) close(held);
  if (pathfd >= 0) close(pathfd);
  if (replacement >= 0) close(replacement);
  close(fd);
  XERROR(XGET(fd, "user.base", output, sizeof(output)), EBADF, "closed public fd");
  XERROR(XRAW(fgetxattr, fd, "user.base", output, sizeof(output)), EBADF, "closed raw fd");
  XCheck(unlink(path) == 0 && rmdir(root) == 0, "remove all fixture resources");
  printf("xattr fd contracts: %d checks, %d failures\n", xattr_checks, xattr_failures);
  return xattr_failures;
}
#undef XGET
#undef XSET
#undef XLIST
#undef XRAW
#undef XERROR
