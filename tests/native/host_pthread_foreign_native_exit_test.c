/* CI-only foreign Windows thread exits through both native termination paths. */
#include <pthread.h>
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *set_fn)(pthread_key_t,const void *);
typedef int (__cdecl *clock_fn)(pthread_t,clockid_t *);
static self_fn dll_self;
static set_fn dll_set;
static clock_fn dll_clock;
static pthread_key_t key;
static volatile LONG destroyed;
struct target { HANDLE ready, release; pthread_t host_id,dll_id; clockid_t clock; DWORD exit_code; int use_dll; };
static void destructor(void *value) { if (value) InterlockedIncrement(&destroyed); }
static void run(struct target *t) {
  if (t->use_dll) { t->dll_id=dll_self(); t->host_id=pthread_self(); }
  else { t->host_id=pthread_self(); t->dll_id=dll_self(); }
  if (!t->host_id || t->host_id!=t->dll_id ||
      (t->use_dll ? dll_set(key,t) : pthread_setspecific(key,t)) ||
      dll_clock(t->dll_id,&t->clock) || !SetEvent(t->ready) ||
      WaitForSingleObject(t->release,5000)!=WAIT_OBJECT_0) abort();
}
static DWORD WINAPI win_worker(void *p) { struct target *t=p; run(t); ExitThread(t->exit_code); return 0; }
static unsigned __stdcall crt_worker(void *p) { struct target *t=p; run(t); _endthreadex(t->exit_code); return 0; }
int main(void) {
  DWORD baseline,after; if (!GetProcessHandleCount(GetCurrentProcess(),&baseline)) return 1;
  HMODULE module=LoadLibraryW(L"libwinpthread-1.dll"); if (!module) return 2;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(module,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 2;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+21>=32768) return 2;
  wcscpy(leaf+1,L"libwinpthread-1.dll"); if (_wcsicmp(expected,actual)) return 2;
  dll_self=(self_fn)GetProcAddress(module,"pthread_self"); dll_set=(set_fn)GetProcAddress(module,"pthread_setspecific");
  dll_clock=(clock_fn)GetProcAddress(module,"pthread_getcpuclockid");
  if (!dll_self || !dll_set || !dll_clock || pthread_key_create(&key,destructor)) return 3;
  pthread_t old[4]={0};
  for (int i=0;i<4;++i) {
    struct target t={0}; t.ready=CreateEventW(NULL,TRUE,FALSE,NULL); t.release=CreateEventW(NULL,TRUE,FALSE,NULL);
    t.exit_code=(DWORD)(23+i); t.use_dll=i&1;
    HANDLE thread=(i<2) ? CreateThread(NULL,0,win_worker,&t,0,NULL) : (HANDLE)_beginthreadex(NULL,0,crt_worker,&t,0,NULL);
    if (!t.ready || !t.release || !thread || WaitForSingleObject(t.ready,5000)!=WAIT_OBJECT_0) return 4;
    for (int j=0;j<i;++j) if (old[j]==t.host_id) return 5;
    old[i]=t.host_id;
    clockid_t from_host=123; struct timespec sample;
    if (pthread_getcpuclockid(t.host_id,&from_host) || from_host!=t.clock || clock_gettime(t.clock,&sample) ||
        WaitForSingleObject(thread,0)!=WAIT_TIMEOUT) return 6;
    if (!SetEvent(t.release) || WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0) return 7;
    DWORD actual; if (!GetExitCodeThread(thread,&actual) || actual!=t.exit_code || destroyed!=i+1) return 8;
    from_host=123; if (pthread_getcpuclockid(t.host_id,&from_host)!=ESRCH || from_host!=123) return 9;
    sample.tv_sec=123; sample.tv_nsec=456; errno=0;
    if (clock_gettime(t.clock,&sample)!=-1 || errno!=EINVAL || sample.tv_sec!=123 || sample.tv_nsec!=456) return 10;
    if (!CloseHandle(thread) || !CloseHandle(t.ready) || !CloseHandle(t.release)) return 11;
    printf("PASS probe foreign native exit method=%s attach=%s code=%lu destructor=%ld stale-ESRCH\n",i<2?"ExitThread":"_endthreadex",t.use_dll?"dll":"host",actual,destroyed);
  }
  if (pthread_key_delete(key) || !FreeLibrary(module) || GetModuleHandleW(L"libwinpthread-1.dll") ||
      !GetProcessHandleCount(GetCurrentProcess(),&after) || after>baseline+8) return 12;
  printf("PASS probe foreign native exit handles=%lu/%lu\n",baseline,after);
  return 0;
}
