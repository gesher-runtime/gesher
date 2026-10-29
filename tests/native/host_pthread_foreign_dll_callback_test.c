/* Probe DLL-owned key callback during a real Win32 thread exit. */
#include <pthread.h>
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
typedef struct { HANDLE entered, release; volatile LONG calls, failed; } callback_state;
typedef void (__cdecl *destructor_fn)(void *);
struct worker_state { HANDLE ready, release; pthread_t id; callback_state callback; };
static pthread_key_t old_key, new_key;
static volatile LONG new_calls;
static void replacement(void *v) { if (v==(void *)(uintptr_t)9) InterlockedIncrement(&new_calls); }
static DWORD WINAPI target(void *arg) {
  struct worker_state *s=arg; s->id=pthread_self();
  if (!s->id || pthread_setspecific(old_key,&s->callback) || !SetEvent(s->ready) ||
      WaitForSingleObject(s->release,5000)!=WAIT_OBJECT_0) return 2;
  ExitThread(37); return 0;
}
static DWORD WINAPI reused(void *arg) { (void)arg; if (pthread_setspecific(new_key,(void *)(uintptr_t)9)) return 2; ExitThread(38); return 0; }
int main(void) {
  DWORD before,after; wchar_t path[32768],actual[32768];
  if (!GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  DWORD n=GetModuleFileNameW(NULL,path,32768); if (!n || n>=32768) return 1;
  wchar_t *leaf=wcsrchr(path,L'\\'); if (!leaf || (size_t)(leaf-path)+20>=32768) return 1;
  wcscpy(leaf+1,L"probe-callback.dll"); HMODULE module=LoadLibraryW(path);
  if (!module || !GetModuleFileNameW(module,actual,32768) || _wcsicmp(path,actual)) return 2;
  destructor_fn fn=(destructor_fn)GetProcAddress(module,"probe_destructor");
  struct worker_state s={0}; s.ready=CreateEventW(NULL,TRUE,FALSE,NULL); s.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  s.callback.entered=CreateEventW(NULL,TRUE,FALSE,NULL); s.callback.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!fn || !s.ready || !s.release || !s.callback.entered || !s.callback.release || pthread_key_create(&old_key,fn)) return 3;
  if (!FreeLibrary(module) || !GetModuleHandleW(L"probe-callback.dll")) return 4;
  HANDLE thread=CreateThread(NULL,0,target,&s,0,NULL);
  if (!thread || WaitForSingleObject(s.ready,5000)!=WAIT_OBJECT_0 || !SetEvent(s.release) ||
      WaitForSingleObject(s.callback.entered,5000)!=WAIT_OBJECT_0) return 5;
  MEMORY_BASIC_INFORMATION region;
  if (!VirtualQuery((const void *)fn,&region,sizeof(region)) || region.State!=MEM_COMMIT || region.AllocationBase!=module ||
      pthread_key_delete(old_key) || s.callback.calls!=1 || !SetEvent(s.callback.release) ||
      WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0) return 6;
  DWORD code=0; if (!GetExitCodeThread(thread,&code) || code!=37 || s.callback.failed || s.callback.calls!=1 ||
      GetModuleHandleW(L"probe-callback.dll") || pthread_detach(s.id)!=ESRCH ||
      pthread_key_create(&new_key,replacement) || new_key!=old_key) return 7;
  HANDLE next=CreateThread(NULL,0,reused,NULL,0,NULL);
  if (!next || WaitForSingleObject(next,5000)!=WAIT_OBJECT_0 || !GetExitCodeThread(next,&code) || code!=38 ||
      new_calls!=1 || pthread_key_delete(new_key)) return 8;
  if (!CloseHandle(next) || !CloseHandle(thread) || !CloseHandle(s.ready) || !CloseHandle(s.release) ||
      !CloseHandle(s.callback.entered) || !CloseHandle(s.callback.release) ||
      !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 9;
  printf("PASS probe DLL destructor native exit delete/reuse physical unload handles=%lu/%lu\n",before,after); return 0;
}
