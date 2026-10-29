/* Diagnostic: two independent providers in one process. CI only. */
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
#include <errno.h>
#include <windows.h>

typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *id_fn)(pthread_t, clockid_t *);
typedef int (__cdecl *read_fn)(clockid_t, struct timespec *);
static self_fn dll_self;
static id_fn dll_id;
static clockid_t remote;
static pthread_t remote_thread;
static HANDLE ready, release;
static uint64_t ticks(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static uint64_t cpu(HANDLE h) {
  FILETIME c,e,k,u;
  if (!GetThreadTimes(h,&c,&e,&k,&u)) return UINT64_MAX;
  return ticks(k)+ticks(u);
}
static DWORD WINAPI worker(void *unused) {
  (void)unused;
  remote_thread = dll_self();
  if (dll_id(remote_thread, &remote)) return 1;
  if (!SetEvent(ready) || WaitForSingleObject(release, 5000) != WAIT_OBJECT_0) return 2;
  return 0;
}

int main(void) {
  HMODULE module = LoadLibraryW(L"libwinpthread-1.dll");
  if (!module) return 1;
  wchar_t expected[32768], actual[32768];
  DWORD n=GetModuleFileNameW(NULL,expected,32768), m=GetModuleFileNameW(module,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 11;
  wchar_t *leaf=wcsrchr(expected,L'\\');
  if (!leaf || (size_t)(leaf-expected)+21>=32768) return 12;
  wcscpy(leaf+1,L"libwinpthread-1.dll");
  if (_wcsicmp(expected,actual)) return 13;
  self_fn self = (self_fn)GetProcAddress(module, "pthread_self");
  id_fn id = (id_fn)GetProcAddress(module, "pthread_getcpuclockid");
  read_fn read_clock = (read_fn)GetProcAddress(module, "clock_gettime");
  if (!self || !id || !read_clock) return 2;
  dll_self = self; dll_id = id;
  ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  release = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!ready || !release) return 7;
  HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  if (!thread || WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0) return 8;
  typedef BOOL (WINAPI *compare_fn)(HANDLE, HANDLE);
  HMODULE kernel=GetModuleHandleW(L"kernelbase.dll");
  compare_fn compare=kernel ? (compare_fn)GetProcAddress(kernel,"CompareObjectHandles") : NULL;
  HANDLE duplicate=NULL;
  if (!compare || !DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)) return 20;
  if (!compare(thread,duplicate)) return 21;
  SetLastError(ERROR_SUCCESS);
  if (compare(thread,GetCurrentThread()) || GetLastError()!=ERROR_NOT_SAME_OBJECT || !CloseHandle(duplicate)) return 22;
  clockid_t local, cross_static, cross_dll, same_target;
  struct timespec a, b;
  pthread_t local_thread=pthread_self();
  if (pthread_getcpuclockid(local_thread, &local) ||
      pthread_getcpuclockid(remote_thread, &cross_static) || id(local_thread, &cross_dll)) return 3;
  printf("MIXED_PTHREAD static=%llu dll=%llu cross_static=%ld cross_dll=%ld\n",
         (unsigned long long)local_thread, (unsigned long long)remote_thread,
         (long)cross_static, (long)cross_dll);
  if (local_thread != remote_thread || cross_static != local || cross_dll != remote) return 15;
  pthread_t dll_main=self();
  if (id(dll_main,&same_target) || same_target!=local) return 23;
  printf("MIXED_SAME_THREAD static=%llu dll=%llu same_clock=%ld\n",
         (unsigned long long)local_thread, (unsigned long long)dll_main, (long)same_target);
  ULONGLONG until=GetTickCount64()+100;
  while (GetTickCount64()<until) { }
  uint64_t main_before=cpu(GetCurrentThread()), worker_before=cpu(thread);
  if (clock_gettime(remote, &a) || read_clock(local, &b)) return 4;
  uint64_t main_after=cpu(GetCurrentThread()), worker_after=cpu(thread);
  uint64_t a_ticks=(uint64_t)a.tv_sec*10000000+(uint64_t)a.tv_nsec/100;
  uint64_t b_ticks=(uint64_t)b.tv_sec*10000000+(uint64_t)b.tv_nsec/100;
  if (main_before==UINT64_MAX || worker_before==UINT64_MAX || main_after==UINT64_MAX || worker_after==UINT64_MAX ||
      a_ticks<worker_before || a_ticks>worker_after || b_ticks<main_before || b_ticks>main_after) return 14;
  printf("MIXED_PROVIDER static=%ld dll=%ld same=%d main_tid=%lu worker_tid=%lu static_ticks=%llu dll_ticks=%llu\n",
         (long)local, (long)remote, local == remote, GetCurrentThreadId(), GetThreadId(thread),
         (unsigned long long)a_ticks, (unsigned long long)b_ticks);
  if (local == remote) return 5;
  if (!SetEvent(release) || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 9;
  DWORD status;
  if (!GetExitCodeThread(thread, &status) || status || !CloseHandle(thread) ||
      !CloseHandle(ready) || !CloseHandle(release)) return 10;
  a.tv_sec=123; a.tv_nsec=456; errno=0;
  if (clock_gettime(remote, &a)!=-1 || errno!=EINVAL || a.tv_sec!=123 || a.tv_nsec!=456) return 17;
  b.tv_sec=123; b.tv_nsec=456; errno=0;
  if (read_clock(remote, &b)!=-1 || errno!=EINVAL || b.tv_sec!=123 || b.tv_nsec!=456) return 19;
  if (!FreeLibrary(module)) return 6;
  if (GetModuleHandleW(L"libwinpthread-1.dll") != NULL || clock_gettime(local, &a)) return 16;
  a.tv_sec=123; a.tv_nsec=456; errno=0;
  if (clock_gettime(remote, &a)!=-1 || errno!=EINVAL || a.tv_sec!=123 || a.tv_nsec!=456) return 18;
  puts("PASS shared process clock registry cross-module identity and owner survives DLL unload");
  return 0;
}
