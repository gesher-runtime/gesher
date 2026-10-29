/* Diagnostic: two independent providers in one process. CI only. */
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <windows.h>

typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *id_fn)(pthread_t, clockid_t *);
typedef int (__cdecl *read_fn)(clockid_t, struct timespec *);
static self_fn dll_self;
static id_fn dll_id;
static clockid_t remote;
static HANDLE ready, release;
static DWORD WINAPI worker(void *unused) {
  (void)unused;
  if (dll_id(dll_self(), &remote)) return 1;
  if (!SetEvent(ready) || WaitForSingleObject(release, 5000) != WAIT_OBJECT_0) return 2;
  return 0;
}

int main(void) {
  HMODULE module = LoadLibraryW(L"libwinpthread-1.dll");
  if (!module) return 1;
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
  if (clock_gettime(local, &a) || read_clock(remote, &b)) return 4;
  printf("MIXED_PROVIDER static=%ld dll=%ld same=%d static_ns=%ld dll_ns=%ld\n",
         (long)local, (long)remote, local == remote, a.tv_nsec, b.tv_nsec);
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
