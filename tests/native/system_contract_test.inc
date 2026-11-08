// Shared ELF/host tests for the bionic adapters. A separately opened OFD lock
// witnesses actual range ownership without depending on fork emulation.
#include <limits.h>

static void test_group_queries(void) {
  gid_t groups[2] = {(gid_t)0x11223344, (gid_t)0x55667788};
  check("getgroups zero-size query accepts null", getgroups(0, NULL) == 1);
  errno = 0;
  check("getgroups negative size returns EINVAL without writing",
        getgroups(-1, groups) == -1 && errno == EINVAL && groups[0] == (gid_t)0x11223344);
  errno = 0;
  check("getgroups missing output returns EFAULT", getgroups(1, NULL) == -1 && errno == EFAULT);
  check("getgroups fills the declared synthetic group only",
        getgroups(2, groups) == 1 && groups[0] == getgid() && groups[1] == (gid_t)0x55667788);
  groups[0] = (gid_t)0x11223344;
  check("getgroups exact capacity fills one group", getgroups(1, groups) == 1 && groups[0] == getgid());
}

static int lockf_witness_query(int fd, off_t start, off_t length, struct flock* result) {
  memset(result, 0, sizeof(*result));
  result->l_type = F_WRLCK;
  result->l_whence = SEEK_SET;
  result->l_start = start;
  result->l_len = length;
  return fcntl(fd, F_OFD_GETLK, result);
}

static int lockf_witness_set(int fd, short type, off_t start, off_t length) {
  struct flock request;
  memset(&request, 0, sizeof(request));
  request.l_type = type;
  request.l_whence = SEEK_SET;
  request.l_start = start;
  request.l_len = length;
  return fcntl(fd, F_OFD_SETLK, &request);
}

static void test_lockf_contracts(void) {
  typedef int (*lockf_fn)(int, int, int64_t);
  lockf_fn operation = (lockf_fn)dlsym(RTLD_DEFAULT, "lockf");
  lockf_fn operation64 = (lockf_fn)dlsym(RTLD_DEFAULT, "lockf64");
  check("lockf and lockf64 share the LP64 runtime entry", operation != NULL && operation == operation64);
  if (operation == NULL || operation64 == NULL) return;

  char path[] = "gesher-lockf-XXXXXX";
  int fd = mkstemp(path);
  check("lockf regular-file fixture created", fd >= 0);
  if (fd < 0) return;
  int witness = open(path, O_RDWR);
  int readonly = open(path, O_RDONLY);
  check("lockf independent-description witnesses opened", witness >= 0 && readonly >= 0);
  if (witness < 0 || readonly < 0) goto cleanup;
  {
    struct flock observed;
    check("lockf seeks to range start", lseek(fd, 10, SEEK_SET) == 10);
    check("lockf F_TLOCK acquires requested bytes", operation(fd, F_TLOCK, 8) == 0);
    check("lockf F_TEST accepts its own process lock", operation(fd, F_TEST, 8) == 0);
    check("independent OFD query sees the lockf owner and range",
          lockf_witness_query(witness, 10, 8, &observed) == 0 &&
          observed.l_type == F_WRLCK && observed.l_start == 10 && observed.l_len == 8 &&
          observed.l_pid == getpid());
    check("lockf leaves preceding bytes unlocked",
          lockf_witness_query(witness, 0, 10, &observed) == 0 && observed.l_type == F_UNLCK);
    check("lockf leaves following bytes unlocked",
          lockf_witness_query(witness, 18, 1, &observed) == 0 && observed.l_type == F_UNLCK);

    check("lockf accepts a negative-length partial unlock",
          lseek(fd, 18, SEEK_SET) == 18 && operation64(fd, F_ULOCK, -4) == 0);
    check("lockf partial unlock releases only the requested tail",
          lockf_witness_query(witness, 14, 4, &observed) == 0 && observed.l_type == F_UNLCK);
    check("lockf partial unlock preserves the remaining prefix",
          lockf_witness_query(witness, 10, 4, &observed) == 0 &&
          observed.l_type == F_WRLCK && observed.l_start == 10 && observed.l_len == 4);
    check("lockf releases its remaining range",
          lseek(fd, 10, SEEK_SET) == 10 && operation(fd, F_ULOCK, 4) == 0 &&
          lockf_witness_query(witness, 10, 8, &observed) == 0 && observed.l_type == F_UNLCK);

    int held = lockf_witness_set(witness, F_WRLCK, 20, 8);
    check("independent OFD witness acquires conflict range", held == 0);
    if (held == 0) {
      check("lockf seeks to foreign conflict", lseek(fd, 20, SEEK_SET) == 20);
      errno = 0;
      check("lockf F_TEST reports foreign writer with EACCES",
            operation(fd, F_TEST, 8) == -1 && errno == EACCES);
      errno = 0;
      int conflict = operation(fd, F_TLOCK, 8);
      check("lockf F_TLOCK fails on an independently held range",
            conflict == -1 && (errno == EAGAIN || errno == EACCES));
      check("lockf F_ULOCK does not remove another owner", operation(fd, F_ULOCK, 8) == 0);
      errno = 0;
      check("foreign lock survives another owner's unlock",
            operation(fd, F_TEST, 8) == -1 && errno == EACCES);
      check("OFD witness unlock succeeds", lockf_witness_set(witness, F_UNLCK, 20, 8) == 0);
      check("lockf F_TEST succeeds after foreign unlock", operation(fd, F_TEST, 8) == 0);
    }

    // Android's F_TEST intentionally asks whether a reader would conflict.
    held = lockf_witness_set(witness, F_RDLCK, 30, 8);
    check("independent OFD witness acquires shared range", held == 0);
    if (held == 0) {
      check("Android lockf F_TEST permits a foreign shared lock",
            lseek(fd, 30, SEEK_SET) == 30 && operation(fd, F_TEST, 8) == 0);
      errno = 0;
      int conflict = operation(fd, F_TLOCK, 8);
      check("lockf write acquisition still conflicts with a shared lock",
            conflict == -1 && (errno == EAGAIN || errno == EACCES));
      check("shared OFD witness unlock succeeds", lockf_witness_set(witness, F_UNLCK, 30, 8) == 0);
    }

    check("lockf seeks before unsupported blocking operation", lseek(fd, 40, SEEK_SET) == 40);
    errno = 0;
    check("lockf F_LOCK truthfully reports absent blocking support",
          operation(fd, F_LOCK, 8) == -1 && errno == ENOSYS);
    check("unsupported blocking lock publishes no ownership",
          lockf_witness_query(witness, 40, 8, &observed) == 0 && observed.l_type == F_UNLCK);

    const off_t large_offset = (off_t)UINT64_C(8589934592);
    check("lockf64 retains offsets above 32 bits and zero-to-EOF length",
          lseek(fd, large_offset, SEEK_SET) == large_offset && operation64(fd, F_TLOCK, 0) == 0 &&
          lockf_witness_query(witness, large_offset + 4096, 1, &observed) == 0 &&
          observed.l_type == F_WRLCK && observed.l_start == large_offset && observed.l_len == 0);
    check("lockf64 unlocks its zero-to-EOF range", operation64(fd, F_ULOCK, 0) == 0);

    errno = 0;
    check("lockf rejects write lock on a read-only description",
          operation(readonly, F_TLOCK, 1) == -1 && errno == EBADF);
    check("lockf seeks to validate a negative range", lseek(fd, 0, SEEK_SET) == 0);
    errno = 0;
    check("lockf rejects a range before the beginning of the file",
          operation(fd, F_TLOCK, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("lockf rejects an invalid operation", operation(fd, 99, 1) == -1 && errno == EINVAL);
    errno = 0;
    check("lockf validates descriptor even for unsupported blocking mode",
          operation(-1, F_LOCK, 1) == -1 && errno == EBADF);
    errno = 0;
    check("lockf rejects an invalid descriptor", operation(-1, F_TLOCK, 1) == -1 && errno == EBADF);
  }
cleanup:
  if (readonly >= 0) close(readonly);
  if (witness >= 0) close(witness);
  close(fd);
  unlink(path);
}

static void test_system_contracts(void) {
  test_group_queries();
  test_lockf_contracts();
}
