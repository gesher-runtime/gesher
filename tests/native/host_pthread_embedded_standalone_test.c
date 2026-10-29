/* Standalone native host with a statically embedded DLL provider and no anchors. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
typedef pthread_t (__cdecl *self_fn)(void);
typedef int (__cdecl *set_fn)(pthread_key_t,void *);
typedef int (__cdecl *key_create_fn)(pthread_key_t *,void (*)(void *));
typedef int (__cdecl *key_delete_fn)(pthread_key_t);
static self_fn self; static set_fn set; static pthread_key_t key; static volatile LONG destroyed;
static void destructor(void *p) { if (p) InterlockedIncrement(&destroyed); }
static DWORD WINAPI worker(void *unused) { (void)unused; if (!self() || set(key,(void *)1)) return 2; ExitThread(43); return 0; }
int main(void) {
  DWORD before,after,code; HMODULE host=GetModuleHandleW(NULL);
  if (!host || GetProcAddress(host,"gesher_clock_anchor_v1") || GetProcAddress(host,"gesher_pthread_identity_probe_v6") ||
      GetProcAddress(host,"gesher_sync_probe_v1") || !GetProcessHandleCount(GetCurrentProcess(),&before)) return 1;
  HMODULE dll=LoadLibraryW(L"embedded-provider.dll"); if (!dll) return 2;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(dll,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) return 2;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+22>=32768) return 2;
  wcscpy(leaf+1,L"embedded-provider.dll"); if (_wcsicmp(expected,actual)) return 2;
  self=(self_fn)GetProcAddress(dll,"embedded_self"); set=(set_fn)GetProcAddress(dll,"embedded_set");
  key_create_fn create=(key_create_fn)GetProcAddress(dll,"embedded_key_create"); key_delete_fn remove=(key_delete_fn)GetProcAddress(dll,"embedded_key_delete");
  if (!self || !set || !create || !remove || create(&key,destructor)) return 3;
  HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
  if (!thread || WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0 || !GetExitCodeThread(thread,&code) || code!=43 || destroyed!=1 || remove(key) || !CloseHandle(thread)) return 4;
  if (!FreeLibrary(dll) || GetModuleHandleW(L"embedded-provider.dll") || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 5;
  printf("PASS probe standalone statically embedded DLL foreign exit and unload handles=%lu/%lu\n",before,after); return 0;
}
