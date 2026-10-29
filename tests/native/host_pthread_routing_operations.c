/* CI-only cross-image operation experiment. No general pthread claim. */
#include <pthread.h>
#include <time.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
typedef int (__cdecl *create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (__cdecl *join_fn)(pthread_t, void **);
typedef int (__cdecl *detach_fn)(pthread_t);
typedef int (__cdecl *cancel_fn)(pthread_t);
typedef int (__cdecl *key_create_fn)(pthread_key_t *, void (*)(void *));
typedef int (__cdecl *key_delete_fn)(pthread_key_t);
typedef int (__cdecl *set_fn)(pthread_key_t, const void *);
typedef void *(__cdecl *get_fn)(pthread_key_t);
typedef void (__cdecl *cleanup_fn)(pthread_t);
typedef struct _pthread_cleanup **(__cdecl *clean_fn)(void);
typedef int (__cdecl *once_fn)(pthread_once_t *, void (*)(void));
typedef int (__cdecl *state_fn)(int, int *);
typedef int (__cdecl *kill_fn)(pthread_t, int);
typedef void *(__cdecl *handle_fn)(pthread_t);
typedef int (__cdecl *setname_fn)(pthread_t, const char *);
typedef int (__cdecl *getname_fn)(pthread_t, char *, size_t);
static volatile LONG once_calls;
static void once_callback(void) { InterlockedIncrement(&once_calls); }
static HANDLE ready, release_event, oracle;
typedef pthread_t (__cdecl *self_fn)(void);
static self_fn foreign_self;
static pthread_t foreign_id;
static DWORD WINAPI foreign_worker(void *unused) {
  (void)unused;
  foreign_id=foreign_self();
  if (!foreign_id || !SetEvent(ready) || WaitForSingleObject(release_event,5000)!=WAIT_OBJECT_0) return 1;
  return 0;
}
static HANDLE grow_ready, grow_start;
static DWORD WINAPI grow_worker(void *unused) {
  (void)unused;
  pthread_key_t keys[2048];
  if (!SetEvent(grow_ready) || WaitForSingleObject(grow_start,5000)!=WAIT_OBJECT_0) return 1;
  for (unsigned i=0;i<2048;++i) {
    if (pthread_key_create(&keys[i],NULL)) return 2;
    if ((i%32)==0) Sleep(1);
  }
  for (unsigned i=0;i<2048;++i) if (pthread_key_delete(keys[i])) return 3;
  return 0;
}
static volatile LONG destroyed;
static pthread_key_t key;
static void destructor(void *p) { if (p == &key) InterlockedIncrement(&destroyed); }
static void *worker(void *p) {
  (void)p;
  if (pthread_setspecific(key, &key) || !DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&oracle,0,FALSE,DUPLICATE_SAME_ACCESS)) abort();
  if (!SetEvent(ready) || WaitForSingleObject(release_event,5000)!=WAIT_OBJECT_0) abort();
  pthread_testcancel();
  return (void *)(uintptr_t)7;
}
static int start_events(void) {
  ready=CreateEventW(NULL,TRUE,FALSE,NULL); release_event=CreateEventW(NULL,TRUE,FALSE,NULL); oracle=NULL;
  return ready && release_event;
}
static int end_events(void) {
  return oracle && WaitForSingleObject(oracle,5000)==WAIT_OBJECT_0 && CloseHandle(oracle) && CloseHandle(ready) && CloseHandle(release_event);
}
int main(void) {
  HMODULE dll=LoadLibraryW(L"libwinpthread-1.dll");
  if (!dll) return 1;
  wchar_t expected[32768], actual[32768];
  DWORD n=GetModuleFileNameW(NULL,expected,32768), m=GetModuleFileNameW(dll,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 1;
  wchar_t *leaf=wcsrchr(expected,L'\\');
  if (!leaf || (size_t)(leaf-expected)+21>=32768) return 1;
  wcscpy(leaf+1,L"libwinpthread-1.dll");
  if (_wcsicmp(expected,actual)) return 1;
  create_fn create=(create_fn)GetProcAddress(dll,"pthread_create");
  join_fn join=(join_fn)GetProcAddress(dll,"pthread_join"), tryjoin=(join_fn)GetProcAddress(dll,"_pthread_tryjoin");
  detach_fn detach=(detach_fn)GetProcAddress(dll,"pthread_detach");
  cancel_fn cancel=(cancel_fn)GetProcAddress(dll,"pthread_cancel");
  key_create_fn key_create=(key_create_fn)GetProcAddress(dll,"pthread_key_create");
  key_delete_fn key_delete=(key_delete_fn)GetProcAddress(dll,"pthread_key_delete");
  set_fn set=(set_fn)GetProcAddress(dll,"pthread_setspecific");
  get_fn get=(get_fn)GetProcAddress(dll,"pthread_getspecific");
  clean_fn clean=(clean_fn)GetProcAddress(dll,"pthread_getclean");
  cleanup_fn cleanup=(cleanup_fn)GetProcAddress(dll,"_pthread_cleanup_dest");
  once_fn once=(once_fn)GetProcAddress(dll,"pthread_once");
  state_fn state=(state_fn)GetProcAddress(dll,"pthread_setcancelstate");
  state_fn type=(state_fn)GetProcAddress(dll,"pthread_setcanceltype");
  kill_fn kill=(kill_fn)GetProcAddress(dll,"pthread_kill");
  handle_fn handle=(handle_fn)GetProcAddress(dll,"pthread_gethandle");
  setname_fn setname=(setname_fn)GetProcAddress(dll,"pthread_setname_np");
  getname_fn getname=(getname_fn)GetProcAddress(dll,"pthread_getname_np");
  if (!create || !join || !tryjoin || !detach || !cancel || !key_create || !key_delete || !set || !get || !clean ||
      !once || !state || !type || !kill || !handle || !setname || !getname || !cleanup) return 2;
  pthread_once_t once_control=PTHREAD_ONCE_INIT;
  int previous=-1;
  if (pthread_once(&once_control,once_callback) || once(&once_control,once_callback) || once_calls!=1 ||
      state(PTHREAD_CANCEL_DISABLE,&previous) || previous!=PTHREAD_CANCEL_ENABLE ||
      pthread_setcancelstate(PTHREAD_CANCEL_ENABLE,&previous) || previous!=PTHREAD_CANCEL_DISABLE ||
      pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,&previous) || previous!=PTHREAD_CANCEL_DEFERRED ||
      type(PTHREAD_CANCEL_DEFERRED,&previous) || previous!=PTHREAD_CANCEL_ASYNCHRONOUS) return 2;
  previous=77; errno=EDOM;
  if (state(-1,&previous)!=EINVAL || previous!=77 || errno!=EDOM) return 2;
  errno=EDOM;
  if (kill((pthread_t)0,0)!=ESRCH || errno!=EDOM) return 2;
  void (***dll_dest)(void *)=(void (***)(void *))GetProcAddress(dll,"_pthread_key_dest");
  if (!dll_dest || *dll_dest!=_pthread_key_dest || _pthread_key_dest!=NULL) return 3;
  if (key_create(&key,destructor) || set(key,&key) || pthread_getspecific(key)!=&key ||
      pthread_setspecific(key,NULL) || get(key)!=NULL || clean()!=pthread_getclean()) return 3;
  if (!*dll_dest || *dll_dest!=_pthread_key_dest) return 3;
  printf("PROBE_KEY_DATA host=%p dll=%p equal=%d\n", (void *)_pthread_key_dest, (void *)*dll_dest, _pthread_key_dest==*dll_dest);
  pthread_t id; void *result=NULL;
  if (!start_events() || create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) return 4;
  char thread_name[64];
  if (kill(id,0) || !handle(id) || GetThreadId((HANDLE)handle(id))!=GetThreadId(oracle) ||
      pthread_setname_np(id,"host-name") || getname(id,thread_name,sizeof(thread_name)) || strcmp(thread_name,"host-name") ||
      setname(id,"dll-name") || pthread_getname_np(id,thread_name,sizeof(thread_name)) || strcmp(thread_name,"dll-name") ||
      _pthread_tryjoin(id,NULL)!=EBUSY || !SetEvent(release_event) || pthread_join(id,&result) || result!=(void *)(uintptr_t)7 || !end_events() || destroyed!=1) return 4;
  if (!start_events() || pthread_create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 ||
      tryjoin(id,NULL)!=EBUSY || !SetEvent(release_event) || join(id,&result) || result!=(void *)(uintptr_t)7 || !end_events() || destroyed!=2) return 5;
  if (!start_events() || pthread_create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 ||
      cancel(id) || !SetEvent(release_event) || pthread_join(id,&result) || result!=PTHREAD_CANCELED || !end_events() || destroyed!=3) return 6;
  if (!start_events() || pthread_create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 ||
      detach(id) || !SetEvent(release_event) || !end_events() || destroyed!=4) return 7;
  if (!start_events() || create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 ||
      pthread_cancel(id) || !SetEvent(release_event) || join(id,&result) || result!=PTHREAD_CANCELED || !end_events() || destroyed!=5) return 9;
  if (!start_events() || create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 ||
      pthread_detach(id) || !SetEvent(release_event) || !end_events() || destroyed!=6) return 10;
  if (!start_events() || pthread_create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) return 12;
  cleanup(id);
  if (destroyed!=7 || !SetEvent(release_event) || pthread_join(id,&result) || result!=(void *)(uintptr_t)7 || !end_events() || destroyed!=7) return 12;
  puts("PASS probe direct cross-image cleanup helper invokes destructor once");
  if (!start_events()) return 8;
  foreign_self=(self_fn)GetProcAddress(dll,"pthread_self");
  if (!foreign_self) return 8;
  HANDLE foreign=CreateThread(NULL,0,foreign_worker,NULL,0,NULL);
  if (!foreign || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) return 8;
  if (!FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll")) return 8;
  if (!SetEvent(release_event) || WaitForSingleObject(foreign,5000)!=WAIT_OBJECT_0) return 8;
  DWORD exit_code;
  if (!GetExitCodeThread(foreign,&exit_code) || exit_code || !CloseHandle(foreign) || !CloseHandle(ready) || !CloseHandle(release_event)) return 8;
  clockid_t stale=123;
  if (pthread_getcpuclockid(foreign_id,&stale)!=ESRCH || stale!=123) return 8;
  dll=LoadLibraryW(L"libwinpthread-1.dll");
  if (!dll) return 8;
  dll_dest=(void (***)(void *))GetProcAddress(dll,"_pthread_key_dest");
  if (!dll_dest || !*dll_dest || *dll_dest!=_pthread_key_dest) return 8;
  if (!FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll")) return 8;
  grow_ready=CreateEventW(NULL,TRUE,FALSE,NULL); grow_start=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!grow_ready || !grow_start) return 11;
  HANDLE grow=CreateThread(NULL,0,grow_worker,NULL,0,NULL);
  if (!grow || WaitForSingleObject(grow_ready,5000)!=WAIT_OBJECT_0 || !SetEvent(grow_start)) return 11;
  unsigned overlap=0;
  for (unsigned i=0;i<64;++i) {
    dll=LoadLibraryW(L"libwinpthread-1.dll");
    if (!dll) return 11;
    if (WaitForSingleObject(grow,0)==WAIT_TIMEOUT) ++overlap;
    if (!FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll")) return 11;
  }
  if (!overlap || WaitForSingleObject(grow,5000)!=WAIT_OBJECT_0 || !GetExitCodeThread(grow,&exit_code) || exit_code ||
      !CloseHandle(grow) || !CloseHandle(grow_ready) || !CloseHandle(grow_start) || pthread_key_delete(key)) return 11;
  printf("PASS probe DLL data alias admission reload and foreign-thread exit after unload; reloads with growth worker live=%u\n",overlap);
  puts("PASS probe cross-image once cancel-state name kill and borrowed handle");
  puts("PASS probe cross-image create join tryjoin cancel detach TLS cleanup and unload");
  return 0;
}
