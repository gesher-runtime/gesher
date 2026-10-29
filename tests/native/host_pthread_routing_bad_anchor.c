/* CI-only malformed host identity descriptor must terminate before attachment. */
#include <pthread.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
__declspec(dllexport) const void * __cdecl gesher_pthread_identity_probe_v2(void) { return NULL; }
int main(void) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
  puts("CONTROL invalid identity descriptor before pthread_self"); fflush(stdout);
  (void)pthread_self();
  puts("FAIL invalid identity descriptor survived"); return 9;
}
