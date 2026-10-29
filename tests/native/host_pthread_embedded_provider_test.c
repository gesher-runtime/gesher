/* Static host + real -static DLL; foreign thread exits after DLL unload. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>
typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *set_fn)(pthread_key_t,void *);
typedef int (__cdecl *clock_fn)(pthread_t,clockid_t *);
typedef void (** (__cdecl *data_fn)(void))(void *);
typedef int (__cdecl *init_fn)(pthread_mutex_t *);
typedef int (__cdecl *admit_fn)(LONG *,LONG *);
extern int __cdecl gesher_admission_probe_v1(LONG *,LONG *);
static self_fn embedded_self; static set_fn embedded_set; static clock_fn embedded_clock; static data_fn embedded_data; static init_fn embedded_init;
static pthread_key_t key; static volatile LONG destroyed;
struct target { HANDLE ready,release; pthread_t id; clockid_t clock; };
static void destructor(void *v) { if (v) InterlockedIncrement(&destroyed); }
static DWORD WINAPI worker(void *arg) {
  struct target *t=arg; t->id=embedded_self();
  if (!t->id || embedded_set(key,t) || embedded_clock(t->id,&t->clock) || !SetEvent(t->ready) || WaitForSingleObject(t->release,5000)!=WAIT_OBJECT_0) return 1;
  ExitThread(41); return 0;
}
int main(void) {
  DWORD before,after; LONG host_mode=0,host_count=0;
  if (gesher_admission_probe_v1(&host_mode,&host_count) || host_mode!=1 || host_count!=2 ||
      !GetProcessHandleCount(GetCurrentProcess(),&before) || pthread_key_create(&key,destructor)) return 1;
  HMODULE module=LoadLibraryW(L"embedded-provider.dll"); if (!module) return 2;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(module,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 2;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+22>=32768) return 2;
  wcscpy(leaf+1,L"embedded-provider.dll"); if (_wcsicmp(expected,actual)) return 2;
  embedded_self=(self_fn)GetProcAddress(module,"embedded_self"); embedded_set=(set_fn)GetProcAddress(module,"embedded_set");
  embedded_clock=(clock_fn)GetProcAddress(module,"embedded_clock"); embedded_data=(data_fn)GetProcAddress(module,"embedded_key_data"); embedded_init=(init_fn)GetProcAddress(module,"embedded_mutex_init");
  admit_fn admit=(admit_fn)GetProcAddress(module,"gesher_admission_probe_v1"); LONG module_mode=0,module_count=0;
  if (!admit || admit(&module_mode,&module_count) || module_mode!=2 || module_count!=2) return 3;
  void (***slot)(void *)=(void (***)(void *))GetProcAddress(module,"_pthread_key_dest");
  if (!embedded_self || !embedded_set || !embedded_clock || !embedded_data || !embedded_init || !slot ||
      embedded_self()!=pthread_self() || embedded_data()!=_pthread_key_dest || *slot!=_pthread_key_dest) return 3;
  pthread_mutex_t mutex; MEMORY_BASIC_INFORMATION region;
  if (embedded_init(&mutex) || !mutex || !VirtualQuery(*(void **)(uintptr_t)mutex,&region,sizeof(region)) || region.AllocationBase!=GetModuleHandleW(NULL)) return 4;
  struct target t={CreateEventW(NULL,TRUE,FALSE,NULL),CreateEventW(NULL,TRUE,FALSE,NULL),0,0};
  HANDLE thread=CreateThread(NULL,0,worker,&t,0,NULL); if (!t.ready || !t.release || !thread || WaitForSingleObject(t.ready,5000)!=WAIT_OBJECT_0) return 5;
  clockid_t host=0; struct timespec sample;
  if (pthread_getcpuclockid(t.id,&host) || host!=t.clock || clock_gettime(host,&sample) || WaitForSingleObject(thread,0)!=WAIT_TIMEOUT) return 6;
  LONG check_mode=0,check_count=0;
  if (admit(&check_mode,&check_count) || check_mode!=module_mode || check_count!=module_count ||
      gesher_admission_probe_v1(&check_mode,&check_count) || check_mode!=host_mode || check_count!=host_count) return 7;
  if (!FreeLibrary(module) || GetModuleHandleW(L"embedded-provider.dll") || pthread_mutex_lock(&mutex) || pthread_mutex_unlock(&mutex) || pthread_mutex_destroy(&mutex) ||
      !SetEvent(t.release) || WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0) return 7;
  DWORD code; if (!GetExitCodeThread(thread,&code) || code!=41 || destroyed!=1) return 8;
  host=123; if (pthread_getcpuclockid(t.id,&host)!=ESRCH || host!=123 || pthread_key_delete(key) ||
      !CloseHandle(thread) || !CloseHandle(t.ready) || !CloseHandle(t.release) || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 9;
  printf("PASS probe static-host and statically embedded DLL authority foreign exit unload handles=%lu/%lu\n",before,after); return 0;
}
