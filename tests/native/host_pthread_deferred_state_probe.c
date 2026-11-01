/* Diagnostic capability only: actual deferred APIs, native exit and join unwind. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
extern void __cdecl gesher_deferred_gate_v1(pthread_t,void *,void *,pthread_t,void *);
extern LONG __cdecl gesher_deferred_pending_v1(void);
extern int __cdecl gesher_tls_reaper_start_v1(void);
extern int __cdecl gesher_tls_retirement_counts_v1(LONG *,unsigned);
typedef int (__cdecl *cancel_fn)(pthread_t);
typedef int (__cdecl *join_fn)(pthread_t,void **);
typedef int (__cdecl *type_fn)(int,int *);
static cancel_fn dll_cancel;
static join_fn dll_join;
static type_fn dll_type;
static pthread_key_t key;
static volatile LONG self_cleanup,self_dtor,join_cleanup,join_dtor,native_dtor,normal_dtor;
static void require(int ok,const char *what) { if (!ok) { fprintf(stderr,"FAIL deferred %s\n",what); exit(1); } }
static void count(void *arg) { InterlockedIncrement((volatile LONG *)arg); }
static void *self_cancel(void *arg) {
  (void)arg;
  require(!pthread_setspecific(key,(void *)&self_dtor),"self key");
  pthread_cleanup_push(count,(void *)&self_cleanup);
  require(!dll_cancel(pthread_self()),"self deferred request");
  pthread_testcancel();
  require(0,"self testcancel returned");
  pthread_cleanup_pop(0);
  return NULL;
}
struct native_state { HANDLE ready,release; pthread_t id; };
static DWORD WINAPI native_target(void *arg) {
  struct native_state *s=arg; int previous=-1; s->id=pthread_self();
  require(s->id && !pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous) &&
      !pthread_setspecific(key,(void *)&native_dtor),"native attach/state/key");
  require(SetEvent(s->ready) && WaitForSingleObject(s->release,20000)==WAIT_OBJECT_0,"native release");
  ExitThread(61); return 0;
}
static DWORD WINAPI cancel_actor(void *arg) { return (DWORD)dll_cancel(((struct native_state *)arg)->id); }
static void *disabled_normal(void *arg) {
  struct native_state *s=arg;
  require(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL) &&
      !pthread_setspecific(key,(void *)&normal_dtor) && SetEvent(s->ready) &&
      WaitForSingleObject(s->release,20000)==WAIT_OBJECT_0,"disabled normal release");
  return (void *)(uintptr_t)0x5678;
}
struct joined_state { HANDLE release; pthread_t id; };
static void *joined_target(void *arg) {
  struct joined_state *s=arg;
  require(WaitForSingleObject(s->release,20000)==WAIT_OBJECT_0,"joined target release");
  return (void *)(uintptr_t)0x1234;
}
static void *joining_actor(void *arg) {
  struct joined_state *s=arg; void *value=NULL;
  require(!pthread_setspecific(key,(void *)&join_dtor),"joiner key");
  pthread_cleanup_push(count,(void *)&join_cleanup);
  require(!dll_join(s->id,&value),"join unexpectedly returned error");
  pthread_cleanup_pop(0);
  return value;
}
int main(void) {
  setvbuf(stdout,NULL,_IONBF,0); DWORD original=0,before=0,after=0,code=0;
  require(GetProcessHandleCount(GetCurrentProcess(),&original) && !gesher_tls_reaper_start_v1() &&
      GetProcessHandleCount(GetCurrentProcess(),&before) && before==original+2,
      "explicit normal bootstrap owns exactly event and thread handles");
  printf("DEFERRED_TLS_OWNER original=%lu baseline=%lu persistent_handles=2\n",original,before);
  wchar_t path[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,path,32768);
  require(n && n<32768,"executable path"); wchar_t *leaf=wcsrchr(path,L'\\');
  require(leaf && (size_t)(leaf-path)+22<32768,"DLL path capacity"); wcscpy(leaf+1,L"libwinpthread-1.dll");
  HMODULE dll=LoadLibraryW(path);
  require(dll && GetModuleFileNameW(dll,actual,32768) && !_wcsicmp(path,actual),"exact DLL path");
  dll_cancel=(cancel_fn)GetProcAddress(dll,"pthread_cancel"); dll_join=(join_fn)GetProcAddress(dll,"pthread_join");
  dll_type=(type_fn)GetProcAddress(dll,"pthread_setcanceltype");
  require(dll_cancel && dll_join && dll_type,"actual routed API exports");
  int previous=19;
  require(dll_type(PTHREAD_CANCEL_ASYNCHRONOUS,&previous)==ENOTSUP && previous==19,"probe-only async failclosed unchanged output");
  previous=21; require(dll_type(-123,&previous)==EINVAL && previous==21,"invalid type unchanged output");
  require(!dll_type(PTHREAD_CANCEL_DEFERRED,&previous) && previous==PTHREAD_CANCEL_DEFERRED,"default deferred type");
  require(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous) && previous==PTHREAD_CANCEL_ENABLE,"default enabled state");
  require(!pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,&previous) && previous==PTHREAD_CANCEL_DISABLE,"state restore");
  pthread_attr_t attr; require(!pthread_attr_init(&attr),"attributes"); attr.p_state|=PTHREAD_CANCEL_ASYNCHRONOUS;
  pthread_t unchanged=(pthread_t)0x5555;
  require(pthread_create(&unchanged,&attr,self_cancel,NULL)==ENOTSUP && unchanged==(pthread_t)0x5555,"async creation rejected without output publication");
  require(!pthread_attr_destroy(&attr) && !pthread_key_create(&key,count),"key setup");
  pthread_t self; void *value=NULL;
  require(!pthread_create(&self,NULL,self_cancel,NULL) && !dll_join(self,&value) && value==PTHREAD_CANCELED &&
      self_cleanup==1 && self_dtor==1,"self deferred cancellation and exact cleanup/TSD");
  puts("PASS deferred finite state/type outputs and self cancellation cleanup=1 destructor=1");
  struct native_state ns={CreateEventW(NULL,TRUE,FALSE,NULL),CreateEventW(NULL,TRUE,FALSE,NULL),0};
  HANDLE captured=CreateEventW(NULL,TRUE,FALSE,NULL),resume=CreateEventW(NULL,TRUE,FALSE,NULL);
  require(ns.ready && ns.release && captured && resume,"native gates");
  HANDLE native=CreateThread(NULL,0,native_target,&ns,0,NULL);
  require(native && WaitForSingleObject(ns.ready,5000)==WAIT_OBJECT_0,"native ready");
  gesher_deferred_gate_v1(ns.id,captured,resume,0,NULL);
  HANDLE actor=CreateThread(NULL,0,cancel_actor,&ns,0,NULL);
  require(actor && WaitForSingleObject(captured,5000)==WAIT_OBJECT_0 && SetEvent(ns.release) &&
      WaitForSingleObject(native,5000)==WAIT_OBJECT_0 && GetExitCodeThread(native,&code) && code==61 && native_dtor==1 &&
      SetEvent(resume) && WaitForSingleObject(actor,5000)==WAIT_OBJECT_0 && GetExitCodeThread(actor,&code) && code==ESRCH,
      "captured public cancellation resumes after actual native kernel exit");
  gesher_deferred_gate_v1(0,NULL,NULL,0,NULL);
  require(CloseHandle(actor) && CloseHandle(native) && CloseHandle(ns.ready) && CloseHandle(ns.release) &&
      CloseHandle(captured) && CloseHandle(resume),"native owned cleanup");
  puts("PASS deferred public cancel capture crosses native ExitThread61 and returns ESRCH destructor=1");
  require(gesher_deferred_pending_v1()==0,"pending baseline after delivered self cancellation");
  struct native_state pending={CreateEventW(NULL,TRUE,FALSE,NULL),CreateEventW(NULL,TRUE,FALSE,NULL),0};
  require(pending.ready && pending.release,"disabled pending native gates");
  native=CreateThread(NULL,0,native_target,&pending,0,NULL);
  require(native && WaitForSingleObject(pending.ready,5000)==WAIT_OBJECT_0 && !dll_cancel(pending.id) &&
      !dll_cancel(pending.id) && gesher_deferred_pending_v1()==1,"one pending request despite repeated native cancel");
  require(SetEvent(pending.release) && WaitForSingleObject(native,5000)==WAIT_OBJECT_0 &&
      GetExitCodeThread(native,&code) && code==61 && native_dtor==2 && gesher_deferred_pending_v1()==0 &&
      CloseHandle(native) && CloseHandle(pending.ready) && CloseHandle(pending.release),"native exit consumes disabled pending exactly once");
  puts("PASS deferred disabled native ExitThread pending counter=0/1/0 destructor_delta=1");
  pending.ready=CreateEventW(NULL,TRUE,FALSE,NULL); pending.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  require(pending.ready && pending.release && !pthread_create(&pending.id,NULL,disabled_normal,&pending) &&
      WaitForSingleObject(pending.ready,5000)==WAIT_OBJECT_0 && !dll_cancel(pending.id) &&
      !dll_cancel(pending.id) && gesher_deferred_pending_v1()==1,"one pending request despite repeated normal cancel");
  require(SetEvent(pending.release) && !dll_join(pending.id,&value) && value==(void *)(uintptr_t)0x5678 &&
      normal_dtor==1 && gesher_deferred_pending_v1()==0 &&
      CloseHandle(pending.ready) && CloseHandle(pending.release),"normal return consumes disabled pending exactly once");
  puts("PASS deferred disabled normal return pending counter=0/1/0 destructor=1");
  struct joined_state js={CreateEventW(NULL,TRUE,FALSE,NULL),0}; HANDLE joining=CreateEventW(NULL,TRUE,FALSE,NULL);
  require(js.release && joining && !pthread_create(&js.id,NULL,joined_target,&js),"join target");
  gesher_deferred_gate_v1(0,NULL,NULL,js.id,joining);
  pthread_t joiner; require(!pthread_create(&joiner,NULL,joining_actor,&js),"joiner create");
  require(WaitForSingleObject(joining,5000)==WAIT_OBJECT_0,"nonblocking immediate prewait hook");
  HANDLE owned_joiner=NULL;
  require(DuplicateHandle(GetCurrentProcess(),pthread_gethandle(joiner),GetCurrentProcess(),&owned_joiner,0,FALSE,DUPLICATE_SAME_ACCESS),"owned joiner handle");
  DWORD tid=GetThreadId(owned_joiner); require(tid!=0,"joiner native identity");
  printf("JOIN_WAIT_WITNESS tid=%lu\n",tid);
  char response[32]; require(fgets(response,sizeof(response),stdin) && !strcmp(response,"observed\n"),"external actual OS wait witness");
  require(!dll_cancel(joiner) && !pthread_join(joiner,&value) && value==PTHREAD_CANCELED &&
      join_cleanup==1 && join_dtor==1,"joiner canceled with exact outer cleanup/TSD");
  require(_pthread_tryjoin(js.id,&value)==EBUSY && SetEvent(js.release) && !dll_join(js.id,&value) &&
      value==(void *)(uintptr_t)0x1234,"canceled join leaves original target joinable and unconsumed");
  gesher_deferred_gate_v1(0,NULL,NULL,0,NULL);
  require(CloseHandle(owned_joiner) && CloseHandle(js.release) && CloseHandle(joining) && !pthread_key_delete(key),"join owned cleanup");
  require(gesher_deferred_pending_v1()==0,"no pending cancellation accounting after join unwind");
  puts("PASS deferred genuine join wait cancellation unwinds claim/leases cleanup=1 destructor=1 target remains joinable");
  ULONGLONG drain_deadline=GetTickCount64()+10000;
  LONG retirement[8];
  for (;;) {
    require(!gesher_tls_retirement_counts_v1(retirement,8),"retirement owner counts");
    if (!retirement[0]) break;
    require(GetTickCount64()<drain_deadline,"actual kernel retirement owner drain"); Sleep(1);
  }
  require(FreeLibrary(dll) && !GetModuleHandleW(L"libwinpthread-1.dll"),"actual DLL unload");
  require(GetProcessHandleCount(GetCurrentProcess(),&after) && after<=before+8 &&
      after<=original+8,"original total final handle bound includes persistent reaper");
  printf("DEFERRED_TLS_OWNER_FINAL original=%lu baseline=%lu after=%lu queued=0 original_total_bound=8\n",original,before,after);
  printf("PASS deferred-only public APIs and actual DLL unload handles=%lu/%lu\n",before,after);
  return 0;
}
