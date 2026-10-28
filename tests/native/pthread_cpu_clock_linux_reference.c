// CI-only Linux observation. Do not dereference a pthread_t after join:
// pthread_getcpuclockid on a stale libc object is not a portable control.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static atomic_int release_target;
static void* target(void* unused) {
  (void)unused;
  while (!atomic_load(&release_target)) usleep(1000);
  return NULL;
}

int main(void) {
  pthread_t thread;
  clockid_t clock;
  struct timespec sample;
  if (pthread_create(&thread, NULL, target, NULL) != 0) return 1;
  if (pthread_getcpuclockid(thread, &clock) != 0) return 2;
  if (clock_gettime(clock, &sample) != 0 || clock_getres(clock, &sample) != 0) return 3;
  atomic_store(&release_target, 1);
  if (pthread_join(thread, NULL) != 0) return 4;
  errno = 0;
  int time_result = clock_gettime(clock, &sample);
  int time_errno = errno;
  errno = 0;
  int res_result = clock_getres(clock, &sample);
  int res_errno = errno;
  printf("Linux retained pthread CPU clock after join: gettime=%d errno=%d getres=%d errno=%d\n",
         time_result, time_errno, res_result, res_errno);
  // Record exact Linux behavior before selecting the compatibility contract.
  return 0;
}
