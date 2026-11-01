/* Opt-in process reaper test: bootstrap outside loader, DLL callback on native exit. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
extern int __cdecl gesher_reaper_fault_v1(int);
extern int __cdecl gesher_reaper_start_v1(void);
extern int __cdecl gesher_reaper_observe_v1(void *,DWORD *,LONG *);
extern int __cdecl gesher_reaper_stop_v1(void);
typedef void (__cdecl *destructor_fn)(void *);
typedef void (__cdecl *observe_fn)(volatile LONG *);
typedef struct { HANDLE entered,release; volatile LONG calls,failed; } callback_state;
struct target { HANDLE ready,release; pthread_t id; callback_state cb; };
static pthread_key_t key;
static void dummy(void *p) { (void)p; }
static DWORD WINAPI worker(void *p) {
  struct target *t=p; t->id=pthread_self();
  if (!t->id || pthread_setspecific(key,&t->cb) || !SetEvent(t->ready) || WaitForSingleObject(t->release,5000)!=WAIT_OBJECT_0) return 2;
  ExitThread(49); return 0;
}
int main(int argc,char **argv) {
  DWORD before,after,tid=0; LONG records=-1; volatile LONG detach_tid=0; if (!GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  if (argc==2 && !strcmp(argv[1],"--missing")) {
    pthread_key_t out=(pthread_key_t)123; if (pthread_key_create(&out,dummy)!=EAGAIN || out!=(pthread_key_t)123) return 2;
    puts("PASS reaper missing bootstrap rejected without key output change"); return 0;
  }
  if (argc==2 && (!strcmp(argv[1],"--fault-event") || !strcmp(argv[1],"--fault-thread"))) {
    int stage=!strcmp(argv[1],"--fault-event")?1:2;
    if (gesher_reaper_fault_v1(stage) || gesher_reaper_start_v1()!=EAGAIN || gesher_reaper_fault_v1(0) ||
        gesher_reaper_start_v1() || gesher_reaper_observe_v1(NULL,&tid,&records) || !tid || records ||
        gesher_reaper_stop_v1() || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 3;
    printf("PASS reaper bootstrap fault stage=%d rollback and retry handles=%lu/%lu\n",stage,before,after); return 0;
  }
  if (argc!=1 || gesher_reaper_start_v1()) return 4;
  HANDLE done=CreateEventW(NULL,TRUE,FALSE,NULL); if (!done || gesher_reaper_observe_v1(done,&tid,&records) || !tid || records) return 5;
  wchar_t path[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,path,32768); if (!n || n>=32768) return 6;
  wchar_t *leaf=wcsrchr(path,L'\\'); if (!leaf || (size_t)(leaf-path)+20>=32768) return 6;
  wcscpy(leaf+1,L"probe-callback.dll"); HMODULE module=LoadLibraryW(path);
  if (!module || !GetModuleFileNameW(module,actual,32768) || _wcsicmp(path,actual)) return 6;
  destructor_fn fn=(destructor_fn)GetProcAddress(module,"probe_destructor");
  observe_fn observe=(observe_fn)GetProcAddress(module,"probe_observe_detach");
  if (!observe) return 6;
  observe(&detach_tid);
  struct target t={0}; t.ready=CreateEventW(NULL,TRUE,FALSE,NULL); t.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  t.cb.entered=CreateEventW(NULL,TRUE,FALSE,NULL); t.cb.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!fn || !t.ready || !t.release || !t.cb.entered || !t.cb.release || pthread_key_create(&key,fn) ||
      !FreeLibrary(module) || !GetModuleHandleW(L"probe-callback.dll")) return 7;
  HANDLE thread=CreateThread(NULL,0,worker,&t,0,NULL); if (!thread || WaitForSingleObject(t.ready,5000)!=WAIT_OBJECT_0) return 8;
  DWORD target_tid=GetThreadId(thread); if (!target_tid || target_tid==tid || !SetEvent(t.release) || WaitForSingleObject(t.cb.entered,5000)!=WAIT_OBJECT_0) return 9;
  MEMORY_BASIC_INFORMATION region;
  if (!VirtualQuery((const void *)fn,&region,sizeof(region)) || region.State!=MEM_COMMIT || region.AllocationBase!=module ||
      pthread_key_delete(key) || t.cb.calls!=1 || WaitForSingleObject(done,0)!=WAIT_TIMEOUT || !SetEvent(t.cb.release) ||
      WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0 || WaitForSingleObject(done,5000)!=WAIT_OBJECT_0) return 10;
  DWORD code=0; if (!GetExitCodeThread(thread,&code) || code!=49 || t.cb.failed || t.cb.calls!=1 || GetModuleHandleW(L"probe-callback.dll") ||
      gesher_reaper_observe_v1(NULL,&tid,&records) || records || (DWORD)detach_tid!=tid ||
      (DWORD)detach_tid==target_tid || gesher_reaper_stop_v1()) return 11;
  if (!CloseHandle(thread) || !CloseHandle(t.ready) || !CloseHandle(t.release) || !CloseHandle(t.cb.entered) ||
      !CloseHandle(t.cb.release) || !CloseHandle(done) || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 12;
  printf("PASS reaper deferred DLL release after native callback and exit detach=%lu reaper=%lu target=%lu handles=%lu/%lu\n",(DWORD)detach_tid,tid,target_tid,before,after); return 0;
}
