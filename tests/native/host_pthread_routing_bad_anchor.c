/* CI-only malformed host identity descriptor must terminate before attachment. */
#include <pthread.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
__declspec(dllexport) const void * __cdecl gesher_pthread_identity_probe_v6(void) { return NULL; }
int main(int argc, char **argv) {
  int embedded=argc==2 && !strcmp(argv[1],"--embedded"); if (argc!=1 && !embedded) return 2;
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
  puts(embedded?"CONTROL invalid identity descriptor before embedded DLL load":"CONTROL invalid identity descriptor before pthread_self"); fflush(stdout);
  HMODULE module=LoadLibraryW(embedded?L"embedded-provider.dll":L"libwinpthread-1.dll");
  if (module) {
    pthread_t (__cdecl *self)(void)=(pthread_t (__cdecl *)(void))GetProcAddress(module,embedded?"embedded_self":"pthread_self");
    if (self) (void)self();
  }
  puts("FAIL invalid identity descriptor survived"); return 9;
}
