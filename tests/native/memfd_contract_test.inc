// Real Android imports: the public API and architecture's raw syscall must
// reach the same anonymous-file implementation. API 26 builds use dlsym for
// memfd_create, whose public declaration arrived later.
#include <linux/memfd.h>
#include <sys/syscall.h>
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

typedef int (*memfd_create_fn)(const char*, unsigned int);
static long (*volatile memfd_raw_call)(long, ...) = syscall;

static int memfd_from_syscall(const char* name, unsigned int flags) {
  return (int)memfd_raw_call(SYS_memfd_create, name, flags);
}

static void memfd_reject(memfd_create_fn create, const char* name, unsigned flags,
                         int expected, const char* label) {
  errno = 0;
  int fd = create(name, flags);
  check(label, fd == -1 && errno == expected);
  if (fd >= 0) close(fd);
}

static void test_memfd_route(memfd_create_fn create, const char* route) {
  printf("memfd route: %s\n", route);
  const unsigned flags[] = {0, MFD_CLOEXEC, MFD_EXEC, MFD_CLOEXEC | MFD_EXEC};
  for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
    int fd = create("gesher-memfd-contract", flags[i]);
    check("memfd supported flags create real storage", fd >= 0);
    if (fd < 0) continue;
    struct stat info;
    check("memfd starts empty, regular and has exact CLOEXEC state",
          fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == 0 &&
          fcntl(fd, F_GETFD) == ((flags[i] & MFD_CLOEXEC) ? FD_CLOEXEC : 0));
    const char bytes[] = "memfd actual bytes";
    char readback[sizeof(bytes)] = {0};
    check("memfd holds bytes and positional reads preserve its cursor",
          write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes) &&
          lseek(fd, 5, SEEK_SET) == 5 &&
          pread(fd, readback, sizeof(readback), 0) == (ssize_t)sizeof(bytes) &&
          memcmp(readback, bytes, sizeof(bytes)) == 0 && lseek(fd, 0, SEEK_CUR) == 5);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    check("memfd can resize actual storage", ftruncate(fd, (off_t)page) == 0 &&
          fstat(fd, &info) == 0 && info.st_size == (off_t)page);
    void* mapping = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    check("memfd default and EXEC permit executable file mappings",
          mapping != MAP_FAILED && memcmp(mapping, bytes, sizeof(bytes)) == 0);
    if (mapping != MAP_FAILED) check("memfd executable mapping releases", munmap(mapping, page) == 0);
    int alias = dup(fd);
    check("memfd duplicate retains the shared description",
          alias >= 0 && lseek(alias, 7, SEEK_SET) == 7 && lseek(fd, 0, SEEK_CUR) == 7);
    check("memfd primary descriptor closes", close(fd) == 0);
    if (alias >= 0) {
      memset(readback, 0, sizeof(readback));
      int alive = pread(alias, readback, sizeof(readback), 0) == (ssize_t)sizeof(bytes) &&
                  memcmp(readback, bytes, sizeof(bytes)) == 0;
      int closed = close(alias) == 0;
      check("memfd remains live until its final descriptor closes", alive && closed);
      errno = 0;
      check("memfd final close retires descriptor", fcntl(alias, F_GETFD) == -1 && errno == EBADF);
    }
  }

  int empty = create("", 0);
  check("memfd empty diagnostic name is valid", empty >= 0);
  if (empty >= 0) close(empty);
  char name[251]; memset(name, 'n', 249); name[249] = '\0';
  int maximum = create(name, 0);
  check("memfd 249-byte diagnostic name is valid", maximum >= 0);
  if (maximum >= 0) close(maximum);
  name[249] = 'n'; name[250] = '\0';
  memfd_reject(create, name, 0, EINVAL, "memfd rejects a 250-byte name");
  memfd_reject(create, NULL, 0, EFAULT, "memfd null name is EFAULT");
  memfd_reject(create, (const char*)(uintptr_t)1, 0, EFAULT, "memfd inaccessible name is EFAULT");
  memfd_reject(create, NULL, 0x20U, EINVAL, "memfd invalid flags precede name access");
  memfd_reject(create, "flags", 0x80000000U, EINVAL, "memfd huge-size bits require HUGETLB");
  memfd_reject(create, "flags", MFD_EXEC | MFD_NOEXEC_SEAL, EINVAL,
               "memfd EXEC and NOEXEC_SEAL conflict");
  // These are explicit backend boundaries, not claims that Android lacks
  // these features. Successful creation must not advertise unenforced policy.
  // (MFD_ALLOW_SEALING is supported: gesher-base stores and enforces seals.)
  const unsigned unsupported[] = {MFD_HUGETLB, MFD_NOEXEC_SEAL, MFD_HUGETLB | (21U << 26)};
  for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
    memfd_reject(create, "unsupported", unsupported[i], EOPNOTSUPP,
                 "memfd unsupported policy never claims success");

  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char* guarded = (char*)mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check("memfd name-probe guard allocation", guarded != MAP_FAILED);
  if (guarded != MAP_FAILED) {
    guarded[page - 1] = '\0';
    int protected_page = mprotect(guarded + page, page, PROT_NONE) == 0;
    check("memfd name-probe guard protection", protected_page);
    if (protected_page) {
      int edge = create(guarded + page - 1, 0);
      check("memfd does not overread a terminated name at page edge", edge >= 0);
      if (edge >= 0) close(edge);
      memfd_reject(create, guarded + page, 0, EFAULT, "memfd protected name does not fault process");
    }
    check("memfd guard allocation releases", munmap(guarded, page * 2) == 0);
  }
}

// Seals (fcntl F_ADD_SEALS/F_GET_SEALS) with Linux's semantics: each seal
// refuses exactly what it forbids, on every write path, through every
// descriptor of the file. (ASharedMemory, which libcutils builds on them, is
// JniTest's: libandroid is not in elf_load_test's host.)
static void test_memfd_seals(memfd_create_fn create) {
  printf("memfd seals\n");
  const long page = sysconf(_SC_PAGESIZE);
  char buf[16] = "sealed";
  int fd = create("seals", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  check("memfd MFD_ALLOW_SEALING is created", fd >= 0);
  if (fd < 0) return;
  check("a sealable memfd starts without seals", fcntl(fd, F_GET_SEALS) == 0);
  check("it is sized", ftruncate(fd, page) == 0);
  errno = 0;
  check("unknown seal bits are EINVAL", fcntl(fd, F_ADD_SEALS, 0x400) == -1 && errno == EINVAL);
  check("F_SEAL_GROW | F_SEAL_SHRINK", fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) == 0 &&
                                           fcntl(fd, F_GET_SEALS) == (F_SEAL_GROW | F_SEAL_SHRINK));
  errno = 0;
  check("F_SEAL_GROW refuses a larger ftruncate", ftruncate(fd, page * 2) == -1 && errno == EPERM);
  errno = 0;
  check("F_SEAL_SHRINK refuses a smaller ftruncate", ftruncate(fd, page / 2) == -1 && errno == EPERM);
  check("the same size is allowed", ftruncate(fd, page) == 0);
  check("a write inside the size is allowed", pwrite(fd, buf, 6, 0) == 6);
  errno = 0;
  check("F_SEAL_GROW refuses a write past the end", pwrite(fd, buf, 6, page - 2) == -1 && errno == EPERM);
  check("fallocate inside the size is allowed", fallocate(fd, 0, 0, page) == 0);
  errno = 0;
  check("F_SEAL_GROW refuses fallocate past the end", fallocate(fd, 0, 0, page * 2) == -1 && errno == EPERM);
  check("enable append on the shared file description", fcntl(fd, F_SETFL, O_APPEND) == 0);
  errno = 0;
  check("F_SEAL_GROW checks append's actual end, not pwrite's offset",
        pwrite(fd, buf, 1, 0) == -1 && errno == EPERM);
  check("restore ordinary file status", fcntl(fd, F_SETFL, 0) == 0);

  char* writable = (char*)mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  check("a writable shared mapping before the write seal", writable != MAP_FAILED);
  check("F_SEAL_FUTURE_WRITE", fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0 &&
                                   (fcntl(fd, F_GET_SEALS) & F_SEAL_FUTURE_WRITE) != 0);
  errno = 0;
  check("F_SEAL_FUTURE_WRITE refuses write", write(fd, buf, 1) == -1 && errno == EPERM);
  errno = 0;
  check("F_SEAL_FUTURE_WRITE refuses pwrite", pwrite(fd, buf, 1, 0) == -1 && errno == EPERM);
  errno = 0;
  check("F_SEAL_FUTURE_WRITE refuses a writable shared mapping",
        mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM);
  char* reader = (char*)mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
  check("a read-only shared mapping is allowed", reader != MAP_FAILED && reader[0] == 's');
  errno = 0;
  check("which mprotect cannot make writable (EACCES)",
        reader != MAP_FAILED && mprotect(reader, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  char* copy = (char*)mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  check("a private writable mapping is allowed", copy != MAP_FAILED);
  if (copy != MAP_FAILED) { copy[0] = 'x'; munmap(copy, page); }
  if (writable != MAP_FAILED) {
    writable[1] = 'E';
    check("the mapping made before the seal stays writable", reader != MAP_FAILED && reader[1] == 'E');
    munmap(writable, page);
  }
  if (reader != MAP_FAILED) munmap(reader, page);
  int dup_fd = dup(fd);
  errno = 0;
  check("another descriptor of the file sees and obeys the seals",
        dup_fd >= 0 && (fcntl(dup_fd, F_GET_SEALS) & F_SEAL_FUTURE_WRITE) != 0 &&
            write(dup_fd, buf, 1) == -1 && errno == EPERM);
  if (dup_fd >= 0) close(dup_fd);
  FILE* stream = fdopen(dup(fd), "w");
  check("sealed memfd can still be wrapped as a stream", stream != NULL);
  if (stream != NULL) {
    check("unbuffered stream setup", setvbuf(stream, NULL, _IONBF, 0) == 0);
    errno = 0;
    check("the stream write path obeys the same write seal",
          fputc('x', stream) == EOF && errno == EPERM && ferror(stream));
    fclose(stream);
  }
  check("F_SEAL_SEAL", fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0);
  errno = 0;
  check("F_SEAL_SEAL refuses further seals", fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM);
  close(fd);

  fd = create("seal-write", MFD_ALLOW_SEALING);
  check("F_SEAL_WRITE on an unmapped memfd", fd >= 0 && ftruncate(fd, page) == 0 &&
                                                 fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0);
  errno = 0;
  check("F_SEAL_WRITE refuses write", fd >= 0 && write(fd, buf, 1) == -1 && errno == EPERM);
  errno = 0;
  check("F_SEAL_WRITE refuses a writable shared mapping",
        fd >= 0 && mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM);
  if (fd >= 0) close(fd);
  fd = create("seal-write-busy", MFD_ALLOW_SEALING);
  char* busy = fd >= 0 && ftruncate(fd, page) == 0
      ? (char*)mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : (char*)MAP_FAILED;
  errno = 0;
  check("F_SEAL_WRITE with a writable shared mapping is EBUSY",
        busy != MAP_FAILED && fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
  if (busy != MAP_FAILED) munmap(busy, page);
  if (fd >= 0) close(fd);

  fd = create("unsealable", 0);
  errno = 0;
  check("without MFD_ALLOW_SEALING: F_SEAL_SEAL, and no seal can be added",
        fd >= 0 && fcntl(fd, F_GET_SEALS) == F_SEAL_SEAL &&
            fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EPERM);
  if (fd >= 0) close(fd);
  int pipe_fds[2];
  errno = 0;
  check("F_GET_SEALS on a pipe is EINVAL",
        pipe(pipe_fds) == 0 && fcntl(pipe_fds[0], F_GET_SEALS) == -1 && errno == EINVAL);
  close(pipe_fds[0]);
  close(pipe_fds[1]);
}

static void test_memfd_contracts(void) {
  memfd_create_fn create = (memfd_create_fn)dlsym(RTLD_DEFAULT, "memfd_create");
  check("memfd_create is a callable Android export", create != NULL);
  if (create != NULL) test_memfd_route(create, "direct export");
  test_memfd_route(memfd_from_syscall, "raw architecture syscall");
  test_memfd_seals(create != NULL ? create : memfd_from_syscall);
}
