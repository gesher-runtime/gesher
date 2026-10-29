/* CI-only cross-image operation experiment. No general pthread claim. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
typedef int (__cdecl *create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (__cdecl *join_fn)(pthread_t, void **);
typedef int (__cdecl *detach_fn)(pthread_t);
typedef int (__cdecl *cancel_fn)(pthread_t);
typedef int (__cdecl *key_create_fn)(pthread_key_t *, void (*)(void *));
typedef int (__cdecl *key_delete_fn)(pthread_key_t);
typedef int (__cdecl *set_fn)(pthread_key_t, const void *);
typedef void *(__cdecl *get_fn)(pthread_key_t);
typedef struct _pthread_cleanup **(__cdecl *clean_fn)(void);
static volatile LONG destroyed;
static pthread_key_t key;
static HANDLE ready, release_event, oracle;
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
  if (!create || !join || !tryjoin || !detach || !cancel || !key_create || !key_delete || !set || !get || !clean) return 2;
  if (key_create(&key,destructor) || set(key,&key) || pthread_getspecific(key)!=&key ||
      pthread_setspecific(key,NULL) || get(key)!=NULL || clean()!=pthread_getclean()) return 3;
  void (***dll_dest)(void *)=(void (***)(void *))GetProcAddress(dll,"_pthread_key_dest");
  if (!dll_dest) return 3;
  printf("PROBE_KEY_DATA host=%p dll=%p equal=%d\n", (void *)_pthread_key_dest, (void *)*dll_dest, _pthread_key_dest==*dll_dest);
  pthread_t id; void *result=NULL;
  if (!start_events() || create(&id,NULL,worker,NULL) || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 ||
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
  if (key_delete(key) || !FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll")) return 8;
  puts("PASS probe cross-image create join tryjoin cancel detach TLS cleanup and unload");
  return 0;
}
