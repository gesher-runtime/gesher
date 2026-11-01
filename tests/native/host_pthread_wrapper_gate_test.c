/* CI-only exact pthread wrapper unlock/cleanup acquisition race. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
extern int __cdecl gesher_wrapper_probe_gate_v1(void *, void *);
static pthread_key_t key;
static volatile LONG destroyed;
static void destructor(void *value) { if (value==(void *)(uintptr_t)1) InterlockedIncrement(&destroyed); }
static void *worker(void *unused) { (void)unused; return (void *)(uintptr_t)pthread_setspecific(key,(void *)(uintptr_t)1); }
int main(void) {
  DWORD baseline,after; HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL), release=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!entered || !release || !GetProcessHandleCount(GetCurrentProcess(),&baseline) ||
      pthread_key_create(&key,destructor) || gesher_wrapper_probe_gate_v1(entered,release)) return 1;
  pthread_t thread; HANDLE handle=NULL; unsigned long long generation=0;
  if (pthread_create(&thread,NULL,worker,NULL) || WaitForSingleObject(entered,5000)!=WAIT_OBJECT_0 ||
      gesher_winpthread_dup_cpu_handle_v1(thread,(void **)&handle,&generation) || !handle || !generation ||
      WaitForSingleObject(handle,0)!=WAIT_TIMEOUT || pthread_detach(thread) || !SetEvent(release) ||
      WaitForSingleObject(handle,5000)!=WAIT_OBJECT_0 || destroyed!=1) return 2;
  void *unchanged=(void *)(uintptr_t)1; unsigned long long old=2;
  if (gesher_winpthread_dup_cpu_handle_v1(thread,&unchanged,&old)!=ESRCH || unchanged!=(void *)(uintptr_t)1 || old!=2 ||
      gesher_wrapper_probe_gate_v1(NULL,NULL) || pthread_key_delete(key) || !CloseHandle(handle) ||
      !CloseHandle(entered) || !CloseHandle(release) || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>baseline+8) return 3;
  printf("PASS probe wrapper unlocked before cleanup; live detach exact destructor retired ESRCH handles=%lu/%lu\n",baseline,after);
  return 0;
}
