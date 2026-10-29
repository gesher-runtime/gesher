/* Diagnostic: two independent providers in one process. CI only. */
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>

typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *id_fn)(pthread_t, clockid_t *);
typedef int (__cdecl *read_fn)(clockid_t, struct timespec *);
static self_fn dll_self;
static id_fn dll_id;
static clockid_t remote;
static HANDLE ready, release;
static uint64_t ticks(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static uint64_t cpu(HANDLE h) {
  FILETIME c,e,k,u;
  if (!GetThreadTimes(h,&c,&e,&k,&u)) return UINT64_MAX;
  return ticks(k)+ticks(u);
}
static DWORD WINAPI worker(void *unused) {
  (void)unused;
  if (dll_id(dll_self(), &remote)) return 1;
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
  clockid_t local;
  struct timespec a, b;
  if (pthread_getcpuclockid(pthread_self(), &local)) return 3;
  ULONGLONG until=GetTickCount64()+100;
  while (GetTickCount64()<until) { }
  uint64_t main_before=cpu(GetCurrentThread()), worker_before=cpu(thread);
  if (clock_gettime(remote, &a) || read_clock(local, &b)) return 4;
  uint64_t main_after=cpu(GetCurrentThread()), worker_after=cpu(thread);
  uint64_t a_ticks=(uint64_t)a.tv_sec*10000000+(uint64_t)a.tv_nsec/100;
  uint64_t b_ticks=(uint64_t)b.tv_sec*10000000+(uint64_t)b.tv_nsec/100;
  if (main_before==UINT64_MAX || worker_before==UINT64_MAX || main_after==UINT64_MAX || worker_after==UINT64_MAX ||
      a_ticks<main_before || a_ticks>main_after || b_ticks<worker_before || b_ticks>worker_after) return 14;
  printf("MIXED_PROVIDER static=%ld dll=%ld same=%d main_tid=%lu worker_tid=%lu static_ticks=%llu dll_ticks=%llu\n",
         (long)local, (long)remote, local == remote, GetCurrentThreadId(), GetThreadId(thread),
         (unsigned long long)a_ticks, (unsigned long long)b_ticks);
  /* This characterizes the known collision; it does not qualify the provider. */
  if (local != remote) return 5;
  if (!SetEvent(release) || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 9;
  DWORD status;
  if (!GetExitCodeThread(thread, &status) || status || !CloseHandle(thread) ||
      !CloseHandle(ready) || !CloseHandle(release)) return 10;
  if (!FreeLibrary(module)) return 6;
  puts("OBSERVED independent static and DLL namespaces issue the same ID");
  return 0;
}
