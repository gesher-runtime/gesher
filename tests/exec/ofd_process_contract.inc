// Public POSIX contracts, shared by the Android ELF fixtures and a native
// Linux reference runner. All synchronization bytes come from real processes.
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
int pipe2(int fds[2], int flags);

static inline int ofd_wait(pid_t pid) {
  int status = -1;
  return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static inline int ofd_record_writer(const char* path, char marker) {
  int fd = open(path, O_WRONLY | O_APPEND);
  if (fd < 0) return 31;
  if (write(101, "r", 1) != 1) { close(fd); return 32; }
  char ready;
  if (read(100, &ready, 1) != 1 || ready != 'g') { close(fd); return 33; }
  for (int i = 0; i < 128; ++i) {
    char record[64];
    memset(record, marker, sizeof(record));
    record[1] = (char)('0' + i / 100);
    record[2] = (char)('0' + (i / 10) % 10);
    record[3] = (char)('0' + i % 10);
    off_t before = lseek(fd, 0, SEEK_CUR);
    struct iovec parts[] = {{record, 17}, {record + 17, sizeof(record) - 17}};
    ssize_t written = i % 3 == 0 ? write(fd, record, sizeof(record)) :
                      i % 3 == 1 ? writev(fd, parts, 2) : pwritev(fd, parts, 2, 0);
    if (written != (ssize_t)sizeof(record)) { close(fd); return 34; }
    if (i % 3 == 2 && lseek(fd, 0, SEEK_CUR) != before) { close(fd); return 36; }
  }
  return close(fd) == 0 ? 0 : 35;
}

// -1 means this is not one of this corpus's child modes.
static inline int ofd_process_child(int argc, char** argv) {
  if (argc >= 4 && strcmp(argv[1], "ofd-append") == 0) {
    return ofd_record_writer(argv[2], argv[3][0]);
  }
  if (argc >= 3 && strcmp(argv[1], "ofd-grandchild") == 0) {
    int fd = atoi(argv[2]);
    if ((fcntl(fd, F_GETFL) & (O_APPEND | O_NONBLOCK)) != O_APPEND) return 40;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0 || lseek(fd, 2, SEEK_SET) != 2 || write(fd, "C", 1) != 1) return 41;
    return 0;
  }
  if (argc < 7 || strcmp(argv[1], "ofd-child") != 0) return -1;
  const char* self = argv[2];
  int file = atoi(argv[3]), alias = atoi(argv[4]), cloexec = atoi(argv[5]), independent = atoi(argv[6]);
  int wanted = O_RDWR | O_APPEND | O_NONBLOCK;
  if ((fcntl(file, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) != wanted ||
      (fcntl(alias, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) != wanted) return 50;
  errno = 0;
  if (fcntl(cloexec, F_GETFD) != -1 || errno != EBADF) return 51;
  if (fcntl(independent, F_GETFL) & (O_APPEND | O_NONBLOCK)) return 52;
  if (lseek(alias, 0, SEEK_CUR) != 1 || write(file, "A", 1) != 1 ||
      lseek(alias, 0, SEEK_CUR) != 5) return 53;
  if (fcntl(alias, F_SETFL, 0) != 0 || write(101, "r", 1) != 1) return 54;
  char ready;
  if (read(100, &ready, 1) != 1 || ready != 'g') return 55;
  if ((fcntl(file, F_GETFL) & (O_APPEND | O_NONBLOCK)) != O_APPEND ||
      lseek(file, 0, SEEK_SET) != 0 || write(file, "B", 1) != 1) return 56;
  char alias_arg[24];
  snprintf(alias_arg, sizeof(alias_arg), "%d", alias);
  char* next[] = {(char*)self, (char*)"ofd-grandchild", alias_arg, NULL};
  pid_t pid = -1;
  if (posix_spawn(&pid, self, NULL, NULL, next, environ) != 0 || ofd_wait(pid) != 0) return 57;
  if ((fcntl(file, F_GETFL) & (O_APPEND | O_NONBLOCK)) != O_NONBLOCK || lseek(file, 0, SEEK_CUR) != 3) return 58;
  return 0;
}

#ifdef GESHER_OFD_PROCESS_PARENT
static void ofd_add_control(posix_spawn_file_actions_t* actions, const int go[2], const int ready[2]) {
  posix_spawn_file_actions_init(actions);
  posix_spawn_file_actions_adddup2(actions, go[0], 100);
  posix_spawn_file_actions_adddup2(actions, ready[1], 101);
  posix_spawn_file_actions_addclose(actions, go[0]);
  posix_spawn_file_actions_addclose(actions, go[1]);
  posix_spawn_file_actions_addclose(actions, ready[0]);
  posix_spawn_file_actions_addclose(actions, ready[1]);
}

static void ofd_shared_process_contract(const char* child) {
  char path[] = "gesher-ofd-spawn-XXXXXX";
  int file = mkstemp(path), alias = -1, cloexec = -1, independent = -1;
  int go[2] = {-1, -1}, ready[2] = {-1, -1};
  pid_t pid = -1;
  int setup = file >= 0 && write(file, "base", 4) == 4 && lseek(file, 1, SEEK_SET) == 1 &&
      fcntl(file, F_SETFL, O_APPEND | O_NONBLOCK) == 0;
  if (setup) alias = fcntl(file, F_DUPFD, 7100);  // above the UCRT startup-block limit
  if (setup) cloexec = fcntl(file, F_DUPFD_CLOEXEC, 0);
  if (setup) independent = open(path, O_RDWR);
  setup = setup && alias >= 7100 && cloexec >= 0 && independent >= 0 &&
      pipe2(go, O_CLOEXEC) == 0 && pipe2(ready, O_CLOEXEC) == 0;
  ok("OFD process setup includes a high descriptor and CLOEXEC alias", setup, "setup");
  if (setup) {
    posix_spawn_file_actions_t actions;
    ofd_add_control(&actions, go, ready);
    char fd_arg[24], alias_arg[24], close_arg[24], independent_arg[24];
    snprintf(fd_arg, sizeof(fd_arg), "%d", file);
    snprintf(alias_arg, sizeof(alias_arg), "%d", alias);
    snprintf(close_arg, sizeof(close_arg), "%d", cloexec);
    snprintf(independent_arg, sizeof(independent_arg), "%d", independent);
    char* args[] = {(char*)child, (char*)"ofd-child", (char*)child, fd_arg, alias_arg,
                    close_arg, independent_arg, NULL};
    int spawned = posix_spawn(&pid, child, &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(go[0]); go[0] = -1;
    close(ready[1]); ready[1] = -1;
    char response = 0;
    int signalled = spawned == 0 && read(ready[0], &response, 1) == 1 && response == 'r';
    ok("spawn preserves flags, aliases, shared position and CLOEXEC", signalled, "child setup");
    ok("child F_SETFL reaches the parent but not an independent open",
       signalled && (fcntl(file, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0 &&
           (fcntl(independent, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0 &&
           lseek(alias, 0, SEEK_CUR) == 5, "shared description");
    int changed = signalled && lseek(file, 0, SEEK_SET) == 0 && write(file, "X", 1) == 1 &&
        fcntl(file, F_SETFL, O_APPEND) == 0;
    if (signalled) { ssize_t sent = write(go[1], changed ? "g" : "e", 1); (void)sent; }
    close(go[1]); go[1] = -1;
    int status = spawned == 0 ? ofd_wait(pid) : -1;
    ok("parent F_SETFL reaches the child and a real grandchild", changed && status == 0, "child status");
    ok("grandchild F_SETFL and seek reach both parent aliases",
       status == 0 && (fcntl(alias, F_GETFL) & (O_APPEND | O_NONBLOCK)) == O_NONBLOCK &&
           lseek(file, 0, SEEK_CUR) == 3, "shared description after grandchild exit");
    char actual[7] = {0};
    ok("cross-process append and overwrite transfer the exact real bytes",
       status == 0 && lseek(file, 0, SEEK_SET) == 0 && read(file, actual, 6) == 6 &&
           strcmp(actual, "XaCeAB") == 0, actual);
    close(file); file = open(path, O_RDWR);
    ok("closing and reusing a parent fd does not rebind the surviving alias",
       file >= 0 && (fcntl(file, F_GETFL) & (O_APPEND | O_NONBLOCK)) == 0 &&
           (fcntl(alias, F_GETFL) & (O_APPEND | O_NONBLOCK)) == O_NONBLOCK, "fd reuse");
  }
  if (file >= 0) close(file);
  if (alias >= 0) close(alias);
  if (cloexec >= 0) close(cloexec);
  if (independent >= 0) close(independent);
  for (int i = 0; i < 2; ++i) { if (go[i] >= 0) close(go[i]); if (ready[i] >= 0) close(ready[i]); }
  unlink(path);
}

static void ofd_atomic_append_contract(const char* child) {
  char path[] = "gesher-ofd-append-XXXXXX";
  int file = mkstemp(path), go[2] = {-1, -1}, ready[2] = {-1, -1};
  pid_t first = -1, second = -1;
  int setup = file >= 0 && pipe2(go, O_CLOEXEC) == 0 && pipe2(ready, O_CLOEXEC) == 0;
  ok("atomic append test opens a real shared file", setup, "setup");
  if (setup) {
    posix_spawn_file_actions_t actions;
    ofd_add_control(&actions, go, ready);
    char* a[] = {(char*)child, (char*)"ofd-append", path, (char*)"A", NULL};
    char* b[] = {(char*)child, (char*)"ofd-append", path, (char*)"B", NULL};
    int started_a = posix_spawn(&first, child, &actions, NULL, a, environ);
    int started_b = posix_spawn(&second, child, &actions, NULL, b, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(go[0]); go[0] = -1;
    close(ready[1]); ready[1] = -1;
    char response;
    int signalled = started_a == 0 && started_b == 0 && read(ready[0], &response, 1) == 1 &&
        read(ready[0], &response, 1) == 1;
    ok("two independently opened append writers synchronize through real pipes", signalled, "ready");
    if (signalled) { ssize_t sent = write(go[1], "gg", 2); (void)sent; }
    close(go[1]); go[1] = -1;
    int first_status = started_a == 0 ? ofd_wait(first) : -1;
    int second_status = started_b == 0 ? ofd_wait(second) : -1;
    ok("both independent append writers complete", first_status == 0 && second_status == 0, "writer status");
    struct stat info;
    int exact = fstat(file, &info) == 0 && info.st_size == 2 * 128 * 64;
    unsigned char seen[2][128] = {{0}};
    for (int index = 0; exact && index < 256; ++index) {
      char record[64] = {0};
      exact = read(file, record, sizeof(record)) == (ssize_t)sizeof(record);
      if (!exact) break;
      int writer = record[0] == 'A' ? 0 : record[0] == 'B' ? 1 : -1;
      int sequence = (record[1] - '0') * 100 + (record[2] - '0') * 10 + record[3] - '0';
      if (writer < 0 || sequence < 0 || sequence >= 128 || seen[writer][sequence]++) exact = 0;
      for (int i = 4; exact && i < 64; ++i) if (record[i] != record[0]) exact = 0;
    }
    ok("atomic append loses or overwrites no complete record across processes", exact, "record contents and length");
  }
  if (file >= 0) close(file);
  for (int i = 0; i < 2; ++i) { if (go[i] >= 0) close(go[i]); if (ready[i] >= 0) close(ready[i]); }
  unlink(path);
}
#endif
