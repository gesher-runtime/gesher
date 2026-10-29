/* Probe mixed-image, controlled rwlock lazy initialization contention. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
extern void __cdecl gesher_rwlock_init_gate_v1(void *,void *,void *);
typedef int (__cdecl *rw_fn)(pthread_rwlock_t *);
static rw_fn dll_rd,dll_unlock;
static pthread_rwlock_t rw=PTHREAD_RWLOCK_INITIALIZER;
struct task { int dll,result; };
static DWORD WINAPI run(void *p) {
  struct task *t=p; t->result=t->dll?dll_rd(&rw):pthread_rwlock_tryrdlock(&rw);
  if (!t->result) t->result=t->dll?dll_unlock(&rw):pthread_rwlock_unlock(&rw);
  return 0;
}
int main(void) {
  DWORD before,after; if (!GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  HMODULE dll=LoadLibraryW(L"libwinpthread-1.dll"); if (!dll) return 1;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(dll,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 1;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+21>=32768) return 1;
  wcscpy(leaf+1,L"libwinpthread-1.dll"); if (_wcsicmp(expected,actual)) return 1;
  dll_rd=(rw_fn)GetProcAddress(dll,"pthread_rwlock_tryrdlock"); dll_unlock=(rw_fn)GetProcAddress(dll,"pthread_rwlock_unlock");
  HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL), release=CreateEventW(NULL,TRUE,FALSE,NULL), contended=CreateEventW(NULL,TRUE,FALSE,NULL);
  HANDLE first=NULL,second=NULL; struct task a={0,-1},b={1,-1}; int ok=0;
  if (!dll_rd || !dll_unlock || !entered || !release || !contended) goto done;
  gesher_rwlock_init_gate_v1(entered,release,contended);
  first=CreateThread(NULL,0,run,&a,0,NULL);
  if (!first || WaitForSingleObject(entered,5000)!=WAIT_OBJECT_0) goto done;
  second=CreateThread(NULL,0,run,&b,0,NULL);
  if (!second || WaitForSingleObject(contended,5000)!=WAIT_OBJECT_0 || !SetEvent(release) ||
      WaitForSingleObject(first,5000)!=WAIT_OBJECT_0 || WaitForSingleObject(second,5000)!=WAIT_OBJECT_0 || a.result || b.result || pthread_rwlock_destroy(&rw)) goto done;
  ok=1;
done:
  if (release) SetEvent(release);
  if (first && WaitForSingleObject(first,5000)!=WAIT_OBJECT_0) ExitProcess(97);
  if (second && WaitForSingleObject(second,5000)!=WAIT_OBJECT_0) ExitProcess(97);
  gesher_rwlock_init_gate_v1(NULL,NULL,NULL);
  if (first) CloseHandle(first); if (second) CloseHandle(second);
  if (entered) CloseHandle(entered); if (release) CloseHandle(release); if (contended) CloseHandle(contended);
  if (!FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll") || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 3;
  if (!ok) return 2;
  printf("PASS probe mixed rwlock forced lazy-init contention handles=%lu/%lu\n",before,after); return 0;
}
