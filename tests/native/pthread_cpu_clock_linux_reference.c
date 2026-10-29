// CI-only identity/lifetime diagnostic. Never dereference pthread_t after join.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <gnu/libc-version.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

struct target { atomic_int tid; atomic_int release; atomic_int returning; };
struct task_identity { int present; char state; uint64_t birth; };
struct sample { int result; int error; int64_t nanos; };
static _Noreturn void fail(int code) { fflush(stdout); _Exit(code); }
static int64_t monotonic(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) fail(20);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
// stat field22 is starttime; comm may contain spaces and ')'.
static struct task_identity identity(int tid) {
  char path[80], line[4096];
  snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
  errno = 0;
  FILE* file = fopen(path, "r");
  if (file == NULL) {
    if (errno == ENOENT || errno == ESRCH) return (struct task_identity){0, '-', 0};
    fail(21);
  }
  if (fgets(line, sizeof(line), file) == NULL || fclose(file) != 0) fail(22);
  char* end = strrchr(line, ')');
  if (end == NULL || end[1] != ' ') fail(23);
  char* saved = NULL;
  char* token = strtok_r(end + 2, " ", &saved);
  char state = token != NULL ? token[0] : '?';
  for (int field = 3; field < 22 && token != NULL; ++field) token = strtok_r(NULL, " ", &saved);
  if (token == NULL) fail(24);
  char* tail = NULL;
  uint64_t birth = strtoull(token, &tail, 10);
  if (tail == token || *tail != '\0') fail(25);
  return (struct task_identity){1, state, birth};
}
static int task_count(void) {
  DIR* dir = opendir("/proc/self/task");
  if (dir == NULL) fail(26);
  int count = 0;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') ++count;
  }
  if (closedir(dir) != 0) fail(27);
  return count;
}
static void* worker(void* raw) {
  struct target* target = raw;
  atomic_store(&target->tid, (int)syscall(SYS_gettid));
  while (!atomic_load(&target->release)) usleep(100);
  atomic_store(&target->returning, 1);
  return NULL;
}
static struct sample sample_clock(clockid_t clock, int resolution) {
  struct timespec ts = {0, 0};
  errno = 0;
  int result = resolution ? clock_getres(clock, &ts) : clock_gettime(clock, &ts);
  int error = errno;
  return (struct sample){result, error, (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec};
}
int main(void) {
  alarm(120); // owned whole-process bound even if pthread_join unexpectedly stalls
  struct utsname system;
  if (uname(&system) != 0) fail(1);
  printf("LINUX_PLATFORM kernel=%s version=%s machine=%s glibc=%s pid=%ld\n",
         system.release, system.version, system.machine, gnu_get_libc_version(), (long)getpid());
  int seen[64] = {0};
  unsigned immediate_live = 0, immediate_dead = 0;
  for (int row = 0; row < 64; ++row) {
    struct target target = {0};
    pthread_t thread;
    clockid_t clock;
    if (task_count() != 1 || pthread_create(&thread, NULL, worker, &target) != 0) fail(2);
    int64_t deadline = monotonic() + 5000000000LL;
    int tid;
    while ((tid = atomic_load(&target.tid)) == 0 && monotonic() < deadline) usleep(100);
    if (tid == 0) fail(3);
    for (int old = 0; old < row; ++old) if (seen[old] == tid) {
      printf("LINUX_REUSE row=%d earlier_row=%d tid=%d\n", row, old, tid);
      fail(4);
    }
    seen[row] = tid;
    if (pthread_getcpuclockid(thread, &clock) != 0) fail(5);
    int decoded_tid = ~(clock >> 3); // Linux CPUCLOCK_PID, not a pthread pointer cast
    struct task_identity live = identity(tid);
    struct sample live_time = sample_clock(clock, 0), live_res = sample_clock(clock, 1);
    printf("LINUX_BIRTH row=%d tid=%d decoded=%d clock=%d birth=%" PRIu64
           " state=%c tasks=%d time=%d/%d res=%d/%d\n", row, tid, decoded_tid,
           clock, live.birth, live.state, task_count(), live_time.result, live_time.error,
           live_res.result, live_res.error);
    if (decoded_tid != tid || !live.present || task_count() != 2 ||
        live_time.result != 0 || live_res.result != 0) fail(6);
    int64_t release_at = monotonic();
    atomic_store(&target.release, 1);
    if (pthread_join(thread, NULL) != 0 || atomic_load(&target.returning) != 1) fail(7);
    int64_t joined_at = monotonic();
    // Preserve the failing original's immediate call order before /proc I/O
    // can allow the exiting worker to finish releasing its kernel task.
    struct sample fast_time = sample_clock(clock, 0), fast_res = sample_clock(clock, 1);
    struct task_identity before = identity(tid);
    struct sample early_time = sample_clock(clock, 0), early_res = sample_clock(clock, 1);
    struct task_identity after = identity(tid);
    int64_t sampled_at = monotonic();
    if ((before.present && before.birth != live.birth) || (after.present && after.birth != live.birth)) fail(8);
    printf("LINUX_JOIN row=%d tid=%d birth=%" PRIu64 " release_ns=%" PRId64
           " joined_ns=%" PRId64 " sampled_ns=%" PRId64
           " before=%d/%c/%" PRIu64 " time=%d/%d/%" PRId64
           " res=%d/%d/%" PRId64 " after=%d/%c/%" PRIu64 "\n",
           row, tid, live.birth, release_at, joined_at, sampled_at,
           before.present, before.state, before.birth, early_time.result, early_time.error,
           early_time.nanos, early_res.result, early_res.error, early_res.nanos,
           after.present, after.state, after.birth);
    printf("LINUX_FAST row=%d tid=%d birth=%" PRIu64 " time=%d/%d/%" PRId64
           " res=%d/%d/%" PRId64 "\n", row, tid, live.birth, fast_time.result,
           fast_time.error, fast_time.nanos, fast_res.result, fast_res.error, fast_res.nanos);
    if (fast_time.result == 0) ++immediate_live;
    else if (fast_time.result == -1 && fast_time.error == EINVAL) ++immediate_dead;
    else fail(11);
    // No new target is created until this exact birth disappears AND its
    // clock is rejected. Join, task release and TID reuse are separate phases.
    unsigned observations = 0;
    deadline = monotonic() + 5000000000LL;
    struct task_identity last;
    struct sample late_time, late_res;
    do {
      last = identity(tid);
      late_time = sample_clock(clock, 0);
      late_res = sample_clock(clock, 1);
      ++observations;
      if (last.present && last.birth != live.birth) fail(9);
      if (!last.present && late_time.result == -1 && late_time.error == EINVAL &&
          late_res.result == -1 && late_res.error == EINVAL) break;
      usleep(100);
    } while (monotonic() < deadline);
    int tasks = task_count();
    printf("LINUX_RELEASE row=%d tid=%d birth=%" PRIu64 " elapsed_ns=%" PRId64
           " observations=%u present=%d/%c time=%d/%d res=%d/%d tasks=%d\n",
           row, tid, live.birth, monotonic() - joined_at, observations, last.present,
           last.state, late_time.result, late_time.error, late_res.result, late_res.error, tasks);
    if (last.present || late_time.result != -1 || late_time.error != EINVAL ||
        late_res.result != -1 || late_res.error != EINVAL || tasks != 1) fail(10);
  }
  printf("LINUX_IDENTITY_DIAGNOSTIC rows=64 join_live=%u join_dead=%u controlled_final_release=64 reused_tids=0\n",
         immediate_live, immediate_dead);
  puts("PASS Linux retained pthread CPU clock: 64 identity-bound final releases reject gettime/getres with EINVAL");
  // Scope is the identity-bound final release, never immediate postjoin or
  // Windows qualification. Preserve all FAST and proc phase records above.
  return 0;
}
