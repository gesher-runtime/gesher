/* Probe DLL delay calls use the host cancellation/event owner. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
typedef int (__cdecl *delay32_fn)(const struct _timespec32 *);
typedef int (__cdecl *delay64_fn)(const struct _timespec64 *);
static delay32_fn delay32; static delay64_fn delay64;
struct target { HANDLE ready; int bits; };
static void *worker(void *arg) {
  struct target *t=arg; struct _timespec32 a={10,0}; struct _timespec64 b={10,0};
  if (!SetEvent(t->ready)) return (void *)(uintptr_t)1;
  int result=t->bits==32?delay32(&a):delay64(&b);
  return (void *)(uintptr_t)(result ? 2 : 3);
}
int main(void) {
  HMODULE dll=LoadLibraryW(L"libwinpthread-1.dll"); if (!dll) return 1;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(dll,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 1;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+21>=32768) return 1;
  wcscpy(leaf+1,L"libwinpthread-1.dll"); if (_wcsicmp(expected,actual)) return 1;
  delay32=(delay32_fn)GetProcAddress(dll,"pthread_delay32_np"); delay64=(delay64_fn)GetProcAddress(dll,"pthread_delay64_np");
  if (!delay32 || !delay64) return 2;
  struct _timespec32 a={0,0}; struct _timespec64 b={0,0};
  if (delay32(&a) || delay64(&b)) return 3;
  for (int i=0;i<2;++i) {
    struct target t={CreateEventW(NULL,TRUE,FALSE,NULL),i?64:32}; pthread_t id; HANDLE native=NULL; unsigned long long gen=0; void *result=NULL;
    if (!t.ready || pthread_create(&id,NULL,worker,&t) || WaitForSingleObject(t.ready,5000)!=WAIT_OBJECT_0 ||
        gesher_winpthread_dup_cpu_handle_v1(id,(void **)&native,&gen) || !native || !gen ||
        WaitForSingleObject(native,50)!=WAIT_TIMEOUT || pthread_cancel(id) ||
        WaitForSingleObject(native,5000)!=WAIT_OBJECT_0 || pthread_join(id,&result) || result!=PTHREAD_CANCELED ||
        !CloseHandle(native) || !CloseHandle(t.ready)) return 4;
    printf("PASS probe DLL delay%d host cancellation and original native exit\n",t.bits);
  }
  if (!FreeLibrary(dll) || GetModuleHandleW(L"libwinpthread-1.dll")) return 5;
  puts("PASS probe routed delay32 delay64 cancellation and DLL unload"); return 0;
}
