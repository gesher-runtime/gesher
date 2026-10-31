/* Source-only candidate: CI controller must independently witness joiner OS wait.
   No supplier hooks; target is never detached or concurrently joined. */
#include <pthread.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC
#error This fixture requires a separately named diagnostic supplier role.
#endif
void __cdecl gesher_minimum_join_diag_configure(pthread_t);
LONG __cdecl gesher_minimum_join_diag_snapshot(HANDLE *, DWORD *);
typedef BOOL (WINAPI *compare_fn)(HANDLE, HANDLE);
static HANDLE target_ready, target_release, joiner_ready;
static pthread_t target;
static volatile LONG cleanup_count;

static void fail(const char *why) { fprintf(stderr, "JOIN_CANCEL_FAIL %s\n", why); fflush(stderr); ExitProcess(2); }
static void check(int ok, const char *why) { if (!ok) fail(why); }
static void cleanup(void *ignored) { (void)ignored; InterlockedIncrement(&cleanup_count); }
/* Diagnostic capture only: stack rows do not themselves authorize a wait.
   The controller remains fail-closed until current-binary callsite validation. */
static void capture_joiner_stack(HANDLE thread, DWORD expected_tid) {
#if !defined(_WIN64)
  fail("stack witness requires reviewed x64 ABI");
#else
  DWORD64 pcs[32]; unsigned count = 0; DWORD prior, resumed; int captured = 1;
  CONTEXT context; FILETIME created, exited, kernel, user;
  check(GetThreadId(thread) == expected_tid && GetProcessIdOfThread(thread) == GetCurrentProcessId() &&
        GetThreadTimes(thread, &created, &exited, &kernel, &user), "stack thread identity");
  memset(&context, 0, sizeof context); context.ContextFlags = CONTEXT_FULL;
  prior = SuspendThread(thread);
  check(prior != (DWORD)-1, "stack suspend");
  if (!GetThreadContext(thread, &context)) captured = 0;
  while (captured && count < 32 && context.Rip != 0) {
    DWORD64 image_base = 0; PRUNTIME_FUNCTION function;
    pcs[count++] = context.Rip;
    function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
    if (function) {
      void *handler_data = NULL; DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function,
                       &context, &handler_data, &establisher, NULL);
    } else {
      SIZE_T copied = 0; DWORD64 next = 0;
      if (!context.Rsp || context.Rsp > UINT64_MAX - sizeof next ||
          !ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)context.Rsp,
                             &next, sizeof next, &copied) || copied != sizeof next) {
        captured = 0; break;
      }
      context.Rip = next; context.Rsp += sizeof next;
    }
  }
  resumed = ResumeThread(thread);
  if (prior != 0 || resumed != 1) TerminateProcess(GetCurrentProcess(), 95);
  check(captured && count > 0, "stack capture");
  printf("JOIN_CANCEL_STACK tid=%lu birth_hi=%lu birth_lo=%lu frames=%u suspend_before=%lu resume_before=%lu\n",
         (unsigned long)expected_tid, (unsigned long)created.dwHighDateTime,
         (unsigned long)created.dwLowDateTime, count, (unsigned long)prior, (unsigned long)resumed);
  for (unsigned i = 0; i < count; ++i)
    printf("JOIN_CANCEL_FRAME index=%u pc=%llu\n", i, (unsigned long long)pcs[i]);
#endif
}

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
  HANDLE owned_joiner = NULL, owned_target = NULL;
  void *value = NULL;
  char input[32];
  DWORD tid, wait, observed_tid = 0;
  HANDLE observed_target = NULL;
  ULONGLONG deadline;
  compare_fn compare;
  setvbuf(stdout, NULL, _IONBF, 0);
  target_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  target_release = CreateEventW(NULL, TRUE, FALSE, NULL);
  joiner_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  check(target_ready && target_release && joiner_ready, "events");
  check(pthread_create(&target, NULL, target_main, NULL) == 0, "target create");
  check(WaitForSingleObject(target_ready, 5000) == WAIT_OBJECT_0, "target ready wait");
  gesher_minimum_join_diag_configure(target);
  check(pthread_create(&joiner, NULL, joiner_main, NULL) == 0, "joiner create");
  check(WaitForSingleObject(joiner_ready, 5000) == WAIT_OBJECT_0, "joiner ready wait");
  check(DuplicateHandle(GetCurrentProcess(), pthread_gethandle(joiner), GetCurrentProcess(),
                        &owned_joiner, 0, FALSE, DUPLICATE_SAME_ACCESS), "joiner handle");
  check(DuplicateHandle(GetCurrentProcess(), pthread_gethandle(target), GetCurrentProcess(),
                        &owned_target, 0, FALSE, DUPLICATE_SAME_ACCESS), "target handle");
  check(GetThreadId(owned_target) != 0 && WaitForSingleObject(owned_target, 0) == WAIT_TIMEOUT,
        "actual held target is live");
  tid = GetThreadId(owned_joiner);
  check(tid != 0, "joiner tid");
  deadline = GetTickCount64() + 5000;
  while (gesher_minimum_join_diag_snapshot(&observed_target, &observed_tid) != 1) {
    check(GetTickCount64() < deadline, "diagnostic phase deadline");
    Sleep(1);
  }
  compare = (compare_fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles");
  check(compare && observed_tid == tid && compare(observed_target, owned_target),
        "actual prewait handle object and joiner identity");
  printf("JOIN_CANCEL_PREWAIT tid=%lu target_same_object=1 phase=1\n", (unsigned long)observed_tid);
  capture_joiner_stack(owned_joiner, tid);
  printf("JOIN_CANCEL_TARGET pid=%lu tid=%lu handle=%llu alive=1\n",
         (unsigned long)GetCurrentProcessId(), (unsigned long)GetThreadId(owned_target),
         (unsigned long long)(uintptr_t)owned_target);
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
  check(CloseHandle(owned_joiner) && CloseHandle(owned_target) && CloseHandle(target_ready) && CloseHandle(target_release) && CloseHandle(joiner_ready), "owned cleanup");
  puts("JOIN_CANCEL_PASS cleanup=1 target_joinable=1 target_result=4660");
  return 0;
}
