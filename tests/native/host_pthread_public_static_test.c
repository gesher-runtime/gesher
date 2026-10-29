/* Separate C object, linked directly against the pinned static provider. */
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
static volatile LONG ready, stop;
static clockid_t saved;
static void *worker(void *ignored) {
  (void)ignored;
  if (pthread_getcpuclockid(pthread_self(), &saved)) { InterlockedExchange(&ready, -1); return (void *)1; }
  InterlockedExchange(&ready, 1);
  while (!InterlockedCompareExchange(&stop, 0, 0)) { }
  return NULL;
}
static uint64_t ticks(FILETIME t) { return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; }
static uint64_t cpu(HANDLE h) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return UINT64_MAX;
  return ticks(k) + ticks(u);
}
int main(void) {
  pthread_t thread;
  void *handle = NULL, *result;
  unsigned long long generation;
  int (*read_clock)(clockid_t, struct timespec *) = &clock_gettime;
  int (*read_res)(clockid_t, struct timespec *) = &clock_getres;
  if (pthread_create(&thread, NULL, worker, NULL)) return 1;
  while (!InterlockedCompareExchange(&ready, 0, 0)) Sleep(1);
  if (InterlockedCompareExchange(&ready, 0, 0) != 1) return 7;
  if (gesher_winpthread_dup_cpu_handle_v1(thread, &handle, &generation)) return 2;
  uint64_t before = cpu(handle);
  struct timespec value, resolution;
  if (read_clock(saved, &value) || read_res(saved, &resolution)) return 3;
  uint64_t after = cpu(handle);
  uint64_t sample = (uint64_t)value.tv_sec * 10000000 + (uint64_t)value.tv_nsec / 100;
  if (before == UINT64_MAX || after == UINT64_MAX || sample < before || sample > after ||
      resolution.tv_sec != 0 || resolution.tv_nsec != 100) return 4;
  InterlockedExchange(&stop, 1);
  if (pthread_join(thread, &result) || result || !CloseHandle(handle)) return 5;
  value.tv_sec = 123; value.tv_nsec = 456;
  if (read_clock(saved, &value) != -1 || errno != EINVAL || value.tv_sec != 123 || value.tv_nsec != 456) return 6;
  puts("PASS public-clock static C self-ID cross-thread bracket and retired ID");
  return 0;
}
