/* CI-only mixed static host / supplier DLL synchronization probe. */
#include <pthread.h>
#include <semaphore.h>
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#define DECL(name) static __typeof__(&name) d_##name
DECL(pthread_mutex_init); DECL(pthread_mutex_lock); DECL(pthread_mutex_unlock); DECL(pthread_cond_wait); DECL(pthread_cond_signal);
DECL(pthread_rwlock_rdlock); DECL(pthread_rwlock_tryrdlock); DECL(pthread_rwlock_unlock); DECL(sem_wait); DECL(sem_post);
#define LOAD(name) do { d_##name=(__typeof__(d_##name))GetProcAddress(dll,#name); if (!d_##name) return 2; } while (0)
struct context { pthread_mutex_t mutex; pthread_cond_t cond; pthread_rwlock_t rw; sem_t sem; HANDLE ready,release; volatile LONG cleaned; };
static void unlock_cleanup(void *arg) { struct context *c=arg; if (!pthread_mutex_unlock(&c->mutex)) InterlockedIncrement(&c->cleaned); }
static void *mutex_worker(void *arg) { struct context *c=arg; if (d_pthread_mutex_lock(&c->mutex) || !SetEvent(c->ready) || WaitForSingleObject(c->release,5000)!=WAIT_OBJECT_0 || d_pthread_mutex_unlock(&c->mutex)) return (void *)(uintptr_t)1; return NULL; }
static void *cond_worker(void *arg) {
  struct context *c=arg; int result=0;
  if (pthread_mutex_lock(&c->mutex)) return (void *)(uintptr_t)1;
  pthread_cleanup_push(unlock_cleanup,c);
  if (!SetEvent(c->ready) || d_pthread_cond_wait(&c->cond,&c->mutex)) result=1;
  pthread_cleanup_pop(1);
  return (void *)(uintptr_t)result;
}
static void *sem_worker(void *arg) { struct context *c=arg; if (!SetEvent(c->ready) || d_sem_wait(&c->sem)) return (void *)(uintptr_t)1; return NULL; }
static void *rw_worker(void *arg) { struct context *c=arg; if (d_pthread_rwlock_rdlock(&c->rw) || !SetEvent(c->ready) || WaitForSingleObject(c->release,5000)!=WAIT_OBJECT_0 || d_pthread_rwlock_unlock(&c->rw)) return (void *)(uintptr_t)1; return NULL; }
static int reset(struct context *c) { return ResetEvent(c->ready) && ResetEvent(c->release); }
static int joined(pthread_t id, void *expected) { void *result=(void *)(uintptr_t)99; return !pthread_join(id,&result) && result==expected; }
int main(void) {
  DWORD before,after; if (!GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  HMODULE dll=LoadLibraryW(L"libwinpthread-1.dll"); if (!dll) return 1;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(dll,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 1;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+21>=32768) return 1;
  wcscpy(leaf+1,L"libwinpthread-1.dll"); if (_wcsicmp(expected,actual)) return 1;
  LOAD(pthread_mutex_init); LOAD(pthread_mutex_lock); LOAD(pthread_mutex_unlock); LOAD(pthread_cond_wait); LOAD(pthread_cond_signal);
  LOAD(pthread_rwlock_rdlock); LOAD(pthread_rwlock_tryrdlock); LOAD(pthread_rwlock_unlock); LOAD(sem_wait); LOAD(sem_post);
  struct context c={0}; c.ready=CreateEventW(NULL,TRUE,FALSE,NULL); c.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!c.ready || !c.release || pthread_mutex_init(&c.mutex,NULL) || pthread_cond_init(&c.cond,NULL) ||
      pthread_rwlock_init(&c.rw,NULL) || sem_init(&c.sem,0,0)) return 3;
  pthread_t id;
  if (pthread_create(&id,NULL,mutex_worker,&c) || WaitForSingleObject(c.ready,5000)!=WAIT_OBJECT_0 ||
      pthread_mutex_trylock(&c.mutex)!=EBUSY || !SetEvent(c.release) || !joined(id,NULL) || !reset(&c)) return 4;
  if (pthread_create(&id,NULL,cond_worker,&c) || WaitForSingleObject(c.ready,5000)!=WAIT_OBJECT_0 ||
      pthread_mutex_lock(&c.mutex) || pthread_cond_signal(&c.cond) || pthread_mutex_unlock(&c.mutex) ||
      !joined(id,NULL) || c.cleaned!=1 || !reset(&c)) return 5;
  if (pthread_create(&id,NULL,cond_worker,&c) || WaitForSingleObject(c.ready,5000)!=WAIT_OBJECT_0 ||
      pthread_mutex_lock(&c.mutex) || d_pthread_cond_signal(&c.cond) || pthread_mutex_unlock(&c.mutex) ||
      !joined(id,NULL) || c.cleaned!=2 || !reset(&c)) return 5;
  if (pthread_create(&id,NULL,cond_worker,&c) || WaitForSingleObject(c.ready,5000)!=WAIT_OBJECT_0 ||
      pthread_mutex_lock(&c.mutex) || pthread_cancel(id) || pthread_mutex_unlock(&c.mutex) ||
      !joined(id,PTHREAD_CANCELED) || c.cleaned!=3 || !reset(&c)) return 6;
  if (pthread_create(&id,NULL,sem_worker,&c) || WaitForSingleObject(c.ready,5000)!=WAIT_OBJECT_0 ||
      sem_post(&c.sem) || !joined(id,NULL) || !reset(&c)) return 7;
  if (d_sem_post(&c.sem) || sem_wait(&c.sem)) return 8;
  if (pthread_create(&id,NULL,rw_worker,&c) || WaitForSingleObject(c.ready,5000)!=WAIT_OBJECT_0 ||
      pthread_rwlock_trywrlock(&c.rw)!=EBUSY || !SetEvent(c.release) || !joined(id,NULL) ||
      pthread_rwlock_wrlock(&c.rw) || d_pthread_rwlock_tryrdlock(&c.rw)!=EBUSY || pthread_rwlock_unlock(&c.rw)) return 9;
  pthread_mutex_t live; MEMORY_BASIC_INFORMATION region;
  if (d_pthread_mutex_init(&live,NULL) || !live ||
      !VirtualQuery(*(void **)(uintptr_t)live,&region,sizeof(region)) || region.AllocationBase!=GetModuleHandleW(NULL)) return 10;
  errno=0; if (d_sem_wait(NULL)!=-1 || errno!=EINVAL) return 10;
  if (pthread_cond_destroy(&c.cond) || pthread_mutex_destroy(&c.mutex) || pthread_rwlock_destroy(&c.rw) || sem_destroy(&c.sem) ||
      !CloseHandle(c.ready) || !CloseHandle(c.release) || !FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll") ||
      pthread_mutex_lock(&live) || pthread_mutex_unlock(&live) || pthread_mutex_destroy(&live) ||
      !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 10;
  printf("PASS probe mixed mutex cond-cancel sem rwlock live DLL-init mutex after unload handles=%lu/%lu\n",before,after); return 0;
}
