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
/* ProcessHandleInformation: handle values and object type indices, not inferred ownership. */
typedef struct { HANDLE value; ULONG_PTR handle_count, pointer_count; ULONG access, type, attributes, reserved; } handle_entry;
typedef struct { ULONG_PTR count, reserved; handle_entry entries[1]; } handle_snapshot;
typedef LONG (WINAPI *query_process_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (WINAPI *query_object_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef struct { USHORT length, maximum; WCHAR *buffer; } object_name;
static void check(int ok, const char *s);
static handle_snapshot *previous;
typedef struct { DWORD tid; FILETIME created; } thread_identity;
static thread_identity targets[400];
static size_t target_count;
static int same_time(FILETIME a, FILETIME b) { return a.dwLowDateTime==b.dwLowDateTime && a.dwHighDateTime==b.dwHighDateTime; }
static void inventory(const char *phase) {
  HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
  query_process_fn query=(query_process_fn)GetProcAddress(ntdll,"NtQueryInformationProcess");
  query_object_fn object=(query_object_fn)GetProcAddress(ntdll,"NtQueryObject");
  check(query!=NULL && object!=NULL,"handle inventory query");
  ULONG bytes=65536, required=0; handle_snapshot *snapshot=NULL; LONG status;
  do {
    free(snapshot); snapshot=malloc(bytes); check(snapshot!=NULL,"handle inventory allocation");
    status=query(GetCurrentProcess(),51,snapshot,bytes,&required);
    if (status<0) { check(required>bytes && required<16*1024*1024,"handle inventory size"); bytes=required+4096; }
  } while (status<0);
  check(snapshot->count <= (bytes - 2*sizeof(ULONG_PTR))/sizeof(handle_entry),"handle inventory bounds");
  for (ULONG_PTR i=0;i<snapshot->count;++i) {
    handle_entry *e=&snapshot->entries[i]; int found=0;
    if (previous) for (ULONG_PTR j=0;j<previous->count;++j)
      if (previous->entries[j].value==e->value && previous->entries[j].type==e->type) { found=1; break; }
    if (!found) {
      union { ULONG_PTR alignment; unsigned char bytes[4096]; } storage={0}; ULONG returned=0;
      LONG result=object(e->value,2,storage.bytes,sizeof(storage.bytes),&returned);
      object_name *name=(object_name *)storage.bytes;
      uintptr_t start=(uintptr_t)storage.bytes, end=start+sizeof(storage.bytes), ptr=(uintptr_t)name->buffer;
      if (result>=0 && ptr>=start && ptr<=end && name->length<=end-ptr && name->length<=name->maximum)
        printf("HANDLE_INVENTORY phase=%s value=%p type_index=%lu access=%08lx type=%.*ls\n",phase,e->value,e->type,e->access,(int)(name->length/sizeof(WCHAR)),name->buffer);
      else printf("HANDLE_INVENTORY phase=%s value=%p type_index=%lu access=%08lx query_status=%08lx\n",phase,e->value,e->type,e->access,(unsigned long)result);
      if (result>=0 && ptr>=start && ptr<=end && name->length<=end-ptr && name->length==12 && wcsncmp(name->buffer,L"Thread",6)==0) {
        FILETIME created,exited,kernel,user; DWORD tid=GetThreadId(e->value); int match=0;
        if (tid && GetThreadTimes(e->value,&created,&exited,&kernel,&user)) {
          for (size_t j=0;j<target_count;++j) if (tid==targets[j].tid && same_time(created,targets[j].created)) ++match;
          printf("HANDLE_THREAD phase=%s value=%p tid=%lu current_tid=%lu creation=%08lx%08lx signaled=%lu target_matches=%d\n",
                 phase,e->value,tid,GetCurrentThreadId(),created.dwHighDateTime,created.dwLowDateTime,WaitForSingleObject(e->value,0),match);
        } else printf("HANDLE_THREAD phase=%s value=%p query_failed=%lu\n",phase,e->value,GetLastError());
      }
    }
  }
  free(previous); previous=snapshot;
}
static void check(int ok, const char *s) { if (!ok) { fprintf(stderr,"FAIL handle inventory %s\n",s); abort(); } }
typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *set_fn)(pthread_key_t,const void *);
typedef int (__cdecl *clock_fn)(pthread_t,clockid_t *);
static self_fn dll_self;
static set_fn dll_set;
static clock_fn dll_clock;
static pthread_key_t key;
static volatile LONG destroyed;
struct target { HANDLE ready, release; pthread_t host_id,dll_id; clockid_t clock; DWORD exit_code; int use_dll; };
static void handles(const char *stage) { DWORD n=0; BOOL ok=GetProcessHandleCount(GetCurrentProcess(),&n); printf("CONTROL handle stage=%s ok=%d count=%lu\n",stage,(int)ok,n); }
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
  inventory("original"); HMODULE module=LoadLibraryW(L"libwinpthread-1.dll"); if (!module) return 2; handles("loaded"); inventory("loaded");
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(module,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 2;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+21>=32768) return 2;
  wcscpy(leaf+1,L"libwinpthread-1.dll"); if (_wcsicmp(expected,actual)) return 2;
  dll_self=(self_fn)GetProcAddress(module,"pthread_self"); dll_set=(set_fn)GetProcAddress(module,"pthread_setspecific");
  dll_clock=(clock_fn)GetProcAddress(module,"pthread_getcpuclockid");
  if (!dll_self || !dll_set || !dll_clock || pthread_key_create(&key,destructor)) return 3; handles("key-created"); inventory("key-created");
  pthread_t old[4]={0};
  for (int i=0;i<4;++i) {
    struct target t={0}; t.ready=CreateEventW(NULL,TRUE,FALSE,NULL); t.release=CreateEventW(NULL,TRUE,FALSE,NULL);
    t.exit_code=(DWORD)(23+i); t.use_dll=i&1;
    HANDLE thread=(i<2) ? CreateThread(NULL,0,win_worker,&t,0,NULL) : (HANDLE)_beginthreadex(NULL,0,crt_worker,&t,0,NULL);
    if (!t.ready || !t.release || !thread || WaitForSingleObject(t.ready,5000)!=WAIT_OBJECT_0) return 4;
    FILETIME c={0},e={0},k={0},u={0}; DWORD tid=GetThreadId(thread);
    if (!tid || !GetThreadTimes(thread,&c,&e,&k,&u) || target_count>=400) return 4;
    targets[target_count++]=(thread_identity){tid,c};
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
    if (!CloseHandle(thread) || !CloseHandle(t.ready) || !CloseHandle(t.release)) return 11; handles("iteration-complete"); inventory("iteration-complete");
    printf("PASS probe foreign native exit method=%s attach=%s code=%lu destructor=%ld stale-ESRCH\n",i<2?"ExitThread":"_endthreadex",t.use_dll?"dll":"host",actual,destroyed);
  }
  int delete_result=pthread_key_delete(key);
  BOOL unload_result=FreeLibrary(module);
  HMODULE remaining=GetModuleHandleW(L"libwinpthread-1.dll");
  BOOL count_result=GetProcessHandleCount(GetCurrentProcess(),&after);
  inventory("final");
  printf("CONTROL final delete=%d unload=%d remaining=%p count=%d handles=%lu/%lu\n",
         delete_result,(int)unload_result,(void *)remaining,(int)count_result,baseline,count_result?after:0UL);
  if (delete_result || !unload_result || remaining || !count_result || after>baseline+8) return 12;
  printf("PASS probe foreign native exit handles=%lu/%lu\n",baseline,after);
  return 0;
}
