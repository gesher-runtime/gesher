/* Probe-only: deletion/reuse while a DLL-owned key destructor is executing. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
typedef struct { HANDLE entered, release; volatile LONG calls, failed; } callback_state;
typedef void (__cdecl *destructor_fn)(void *);
typedef int (__cdecl *key_create_fn)(pthread_key_t *, void (*)(void *));
typedef int (__cdecl *key_delete_fn)(pthread_key_t);
typedef int (__cdecl *create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (__cdecl *join_fn)(pthread_t, void **);
static key_create_fn api_key_create=pthread_key_create;
static key_delete_fn api_key_delete=pthread_key_delete;
static create_fn api_create=pthread_create;
static join_fn api_join=pthread_join;
static pthread_key_t old_key, new_key;
static volatile LONG new_calls;
static callback_state state;
static void new_destructor(void *value) { if (value==(void *)(uintptr_t)7) InterlockedIncrement(&new_calls); }
static void *old_worker(void *unused) { (void)unused; return (void *)(uintptr_t)pthread_setspecific(old_key,&state); }
static void *new_worker(void *unused) { (void)unused; return (void *)(uintptr_t)pthread_setspecific(new_key,(void *)(uintptr_t)7); }
static pthread_key_t loop_key, replaced_key;
static volatile LONG loop_calls, replaced_calls;
static void loop_destructor(void *value) {
  if (value!=(void *)(uintptr_t)9) abort();
  LONG count=InterlockedIncrement(&loop_calls);
  if (count<4 && pthread_setspecific(loop_key,value)) abort();
}
static void replacement_destructor(void *value) { if (value==(void *)(uintptr_t)11) InterlockedIncrement(&replaced_calls); }
static void self_replacing_destructor(void *value) {
  if (value!=(void *)(uintptr_t)10 || api_key_delete(loop_key) ||
      api_key_create(&replaced_key,replacement_destructor) || replaced_key!=loop_key ||
      pthread_setspecific(replaced_key,(void *)(uintptr_t)11)) abort();
}
static void *loop_worker(void *value) { return (void *)(uintptr_t)pthread_setspecific(loop_key,value); }

int main(int argc,char **argv) {
  HMODULE provider=NULL;
  if (argc==2 && strcmp(argv[1],"--dll")==0) {
    provider=LoadLibraryW(L"libwinpthread-1.dll");
    if (!provider) return 1;
    wchar_t expected_provider[32768], actual_provider[32768];
    DWORD en=GetModuleFileNameW(NULL,expected_provider,32768), an=GetModuleFileNameW(provider,actual_provider,32768);
    if (!en || en>=32768 || !an || an>=32768) return 1;
    wchar_t *provider_leaf=wcsrchr(expected_provider,L'\\');
    if (!provider_leaf || (size_t)(provider_leaf-expected_provider)+21>=32768) return 1;
    wcscpy(provider_leaf+1,L"libwinpthread-1.dll");
    if (_wcsicmp(expected_provider,actual_provider)) return 1;
    api_key_create=(key_create_fn)GetProcAddress(provider,"pthread_key_create");
    api_key_delete=(key_delete_fn)GetProcAddress(provider,"pthread_key_delete");
    api_create=(create_fn)GetProcAddress(provider,"pthread_create");
    api_join=(join_fn)GetProcAddress(provider,"pthread_join");
    if (!api_key_create || !api_key_delete || !api_create || !api_join) return 1;
  } else if (argc!=1) return 1;
  wchar_t path[32768], actual[32768]; DWORD n=GetModuleFileNameW(NULL,path,32768);
  if (!n || n>=32768) return 1;
  wchar_t *leaf=wcsrchr(path,L'\\');
  if (!leaf || (size_t)(leaf-path)+20>=32768) return 1;
  wcscpy(leaf+1,L"probe-callback.dll");
  HMODULE module=LoadLibraryW(path);
  if (!module || !GetModuleFileNameW(module,actual,32768) || _wcsicmp(path,actual)) return 2;
  destructor_fn destructor=(destructor_fn)GetProcAddress(module,"probe_destructor");
  state.entered=CreateEventW(NULL,TRUE,FALSE,NULL); state.release=CreateEventW(NULL,TRUE,FALSE,NULL);
  if (!destructor || !state.entered || !state.release || api_key_create(&old_key,destructor)) return 3;
  pthread_t old_thread,new_thread; void *result=(void *)1;
  if (api_create(&old_thread,NULL,old_worker,NULL) || WaitForSingleObject(state.entered,5000)!=WAIT_OBJECT_0) return 4;
  if (!FreeLibrary(module) || !GetModuleHandleW(L"probe-callback.dll")) return 5;
  if (api_key_delete(old_key) || !GetModuleHandleW(L"probe-callback.dll")) return 6;
  if (api_key_create(&new_key,new_destructor) || new_key!=old_key ||
      api_create(&new_thread,NULL,new_worker,NULL) || api_join(new_thread,&result) || result || new_calls!=1 || state.calls!=1) return 7;
  if (!SetEvent(state.release) || api_join(old_thread,&result) || result || state.calls!=1 || state.failed ||
      GetModuleHandleW(L"probe-callback.dll") || api_key_delete(new_key) ||
      !CloseHandle(state.entered) || !CloseHandle(state.release)) return 8;
  puts("PASS probe deleted key generation retains DLL callback through return and rejects reuse alias");
  if (api_key_create(&loop_key,loop_destructor) || api_create(&old_thread,NULL,loop_worker,(void *)(uintptr_t)9) ||
      api_join(old_thread,&result) || result || loop_calls!=4 || api_key_delete(loop_key)) return 9;
  if (api_key_create(&loop_key,self_replacing_destructor) || api_create(&old_thread,NULL,loop_worker,(void *)(uintptr_t)10) ||
      api_join(old_thread,&result) || result || replaced_calls!=1 || api_key_delete(replaced_key)) return 10;
  puts("PASS probe four destructor passes and self-delete recreate generation");
  if (provider && (!FreeLibrary(provider) || GetModuleHandleW(L"libwinpthread-1.dll"))) return 11;
  return 0;
}
