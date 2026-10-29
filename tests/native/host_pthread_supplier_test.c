/* A separate dynamic supplier client, without Gesher's POSIX overlay. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

_Static_assert(sizeof(pthread_t) == 8, "Pinned public pthread identity width changed");

int main(void) {
  void *handle = NULL;
  unsigned long long generation = 0;
  FILETIME created, exited, kernel, user;
  int result = gesher_winpthread_dup_cpu_handle_v1(pthread_self(), &handle, &generation);
  if (result || !generation || !handle || WaitForSingleObject(handle, 0) != WAIT_TIMEOUT ||
      !GetThreadTimes(handle, &created, &exited, &kernel, &user)) return 1;
  CloseHandle(handle);
  handle = (void *)(uintptr_t)123;
  generation = 456;
  result = gesher_winpthread_dup_cpu_handle_v1((pthread_t)-1, &handle, &generation);
  if (result != ESRCH || handle != (void *)(uintptr_t)123 || generation != 456) return 2;
  puts("PASS host-authority dynamic-export identity ownership invalid-output");
  return 0;
}
