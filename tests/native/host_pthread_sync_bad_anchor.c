/* Reject a malformed synchronization descriptor before a DLL call uses it. */
#include <pthread.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
typedef const unsigned *(__cdecl *getter_fn)(void);
typedef int (__cdecl *lock_fn)(pthread_mutex_t *);
extern const void * __cdecl gesher_sync_probe_v1(void);
int main(void) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
  getter_fn getter=(getter_fn)GetProcAddress(GetModuleHandleW(NULL),"gesher_sync_probe_v1");
  if (!getter) return 1; const unsigned *table=getter();
  if (!table || (const void *)table!=gesher_sync_probe_v1() || table[1]!=1) return 2;
  DWORD previous; if (!VirtualProtect((void *)table,2*sizeof(unsigned),PAGE_READWRITE,&previous)) return 4;
  ((unsigned *)(uintptr_t)table)[1]=0;
  HMODULE dll=LoadLibraryW(L"libwinpthread-1.dll"); if (!dll) return 5;
  lock_fn lock=(lock_fn)GetProcAddress(dll,"pthread_mutex_lock"); if (!lock) return 6;
  puts("CONTROL malformed synchronization descriptor before mutex use"); fflush(stdout);
  pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER; (void)lock(&mutex);
  puts("FAIL malformed synchronization descriptor accepted"); return 7;
}
