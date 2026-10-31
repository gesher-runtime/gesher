/* Source-only candidate: CI controller must independently witness joiner OS wait.
   No supplier hooks; target is never detached or concurrently joined. */
#include <pthread.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE target_ready, target_release, joiner_ready;
static pthread_t target;
static volatile LONG cleanup_count;

static void fail(const char *why) { fprintf(stderr, "JOIN_CANCEL_FAIL %s\n", why); fflush(stderr); ExitProcess(2); }
static void check(int ok, const char *why) { if (!ok) fail(why); }
static void cleanup(void *ignored) { (void)ignored; InterlockedIncrement(&cleanup_count); }
static void *target_main(void *ignored) {
  (void)ignored;
  check(SetEvent(target_ready), "target ready");
  check(WaitForSingleObject(target_release, 30000) == WAIT_OBJECT_0, "target release");
  return (void *)(uintptr_t)0x1234;
}
static void *joiner_main(void *ignored) {
  void *result = NULL;
  int previous_state = -1, previous_type = -1;
  (void)ignored;
  check(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &previous_state) == 0 &&
        previous_state == PTHREAD_CANCEL_ENABLE, "joiner cancellation enabled");
  check(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &previous_type) == 0 &&
        previous_type == PTHREAD_CANCEL_DEFERRED, "joiner deferred type");
  pthread_cleanup_push(cleanup, NULL);
  check(SetEvent(joiner_ready), "joiner ready");
  check(pthread_join(target, &result) == 0, "joiner join");
  pthread_cleanup_pop(0);
  return result;
}
int main(void) {
  pthread_t joiner;
  HANDLE owned_joiner = NULL;
  void *value = NULL;
  char input[32];
  DWORD tid, wait;
  setvbuf(stdout, NULL, _IONBF, 0);
  target_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  target_release = CreateEventW(NULL, TRUE, FALSE, NULL);
  joiner_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  check(target_ready && target_release && joiner_ready, "events");
  check(pthread_create(&target, NULL, target_main, NULL) == 0, "target create");
  check(WaitForSingleObject(target_ready, 5000) == WAIT_OBJECT_0, "target ready wait");
  check(pthread_create(&joiner, NULL, joiner_main, NULL) == 0, "joiner create");
  check(WaitForSingleObject(joiner_ready, 5000) == WAIT_OBJECT_0, "joiner ready wait");
  check(DuplicateHandle(GetCurrentProcess(), pthread_gethandle(joiner), GetCurrentProcess(),
                        &owned_joiner, 0, FALSE, DUPLICATE_SAME_ACCESS), "joiner handle");
  tid = GetThreadId(owned_joiner);
  check(tid != 0, "joiner tid");
  printf("JOIN_CANCEL_WAIT_WITNESS tid=%lu\n", (unsigned long)tid);
  /* Controller acknowledges only after sampling this exact TID in OS wait. */
  check(fgets(input, sizeof input, stdin) && strcmp(input, "observed\n") == 0, "wait witness");
  check(WaitForSingleObject(owned_joiner, 0) == WAIT_TIMEOUT, "joiner still live");
  check(pthread_cancel(joiner) == 0, "cancel request");
  wait = WaitForSingleObject(owned_joiner, 5000);
  if (wait != WAIT_OBJECT_0) { SetEvent(target_release); fail("join cancellation did not complete while target held"); }
  check(pthread_join(joiner, &value) == 0 && value == PTHREAD_CANCELED, "canceled result");
  check(InterlockedCompareExchange(&cleanup_count, 0, 0) == 1, "cleanup exactly once");
  check(SetEvent(target_release), "release target");
  value = NULL;
  check(pthread_join(target, &value) == 0 && value == (void *)(uintptr_t)0x1234, "target remains joinable");
  check(CloseHandle(owned_joiner) && CloseHandle(target_ready) && CloseHandle(target_release) && CloseHandle(joiner_ready), "owned cleanup");
  puts("JOIN_CANCEL_PASS cleanup=1 target_joinable=1 target_result=4660");
  return 0;
}
