/* CI-only participating host with no identity anchor must fail closed. */
#include <pthread.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
__declspec(dllexport) const void * __cdecl gesher_clock_anchor_v1(void) { return NULL; }
int main(void) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
  puts("CONTROL missing identity descriptor before pthread_self"); fflush(stdout);
  (void)pthread_self();
  puts("FAIL missing identity descriptor survived"); return 9;
}
