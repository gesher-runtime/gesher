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
  const unsigned unsupported[] = {MFD_ALLOW_SEALING, MFD_HUGETLB, MFD_NOEXEC_SEAL,
                                  MFD_ALLOW_SEALING | MFD_EXEC, MFD_HUGETLB | (21U << 26)};
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

static void test_memfd_contracts(void) {
  memfd_create_fn create = (memfd_create_fn)dlsym(RTLD_DEFAULT, "memfd_create");
  check("memfd_create is a callable Android export", create != NULL);
  if (create != NULL) test_memfd_route(create, "direct export");
  test_memfd_route(memfd_from_syscall, "raw architecture syscall");
}
