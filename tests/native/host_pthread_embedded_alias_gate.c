/* Controlled key publication vs static-DLL loader registration. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
extern void __cdecl gesher_alias_gate_v1(void *,void *,void *);
static pthread_key_t grown; static int key_result=-1; static HMODULE loaded;
static DWORD WINAPI grow(void *unused) { (void)unused; key_result=pthread_key_create(&grown,NULL); return 0; }
static DWORD WINAPI load_module(void *unused) { (void)unused; loaded=LoadLibraryW(L"embedded-provider.dll"); return 0; }
int main(void) {
  DWORD before,after; pthread_key_t first; if (!GetProcessHandleCount(GetCurrentProcess(),&before) || pthread_key_create(&first,NULL) || first!=0) return 1;
  HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL), release=CreateEventW(NULL,TRUE,FALSE,NULL), contended=CreateEventW(NULL,TRUE,FALSE,NULL);
  HANDLE worker=NULL,loader=NULL; int ok=0;
  if (!entered || !release || !contended) goto done;
  gesher_alias_gate_v1(entered,release,contended); worker=CreateThread(NULL,0,grow,NULL,0,NULL);
  if (!worker || WaitForSingleObject(entered,5000)!=WAIT_OBJECT_0) goto done;
  loader=CreateThread(NULL,0,load_module,NULL,0,NULL);
  if (!loader || WaitForSingleObject(contended,5000)!=WAIT_OBJECT_0 || !SetEvent(release) ||
      WaitForSingleObject(worker,5000)!=WAIT_OBJECT_0 || WaitForSingleObject(loader,5000)!=WAIT_OBJECT_0 || key_result || grown!=1 || !loaded) goto done;
  wchar_t expected[32768],actual[32768]; DWORD n=GetModuleFileNameW(NULL,expected,32768),m=GetModuleFileNameW(loaded,actual,32768);
  if (!n || n>=32768 || !m || m>=32768) goto done;
  wchar_t *leaf=wcsrchr(expected,L'\\'); if (!leaf || (size_t)(leaf-expected)+22>=32768) goto done;
  wcscpy(leaf+1,L"embedded-provider.dll"); if (_wcsicmp(expected,actual)) goto done;
  void (***slot)(void *)=(void (***)(void *))GetProcAddress(loaded,"_pthread_key_dest");
  if (!slot || *slot!=_pthread_key_dest || pthread_key_delete(grown)) goto done;
  ok=1;
done:
  if (release) SetEvent(release);
  if (worker && WaitForSingleObject(worker,5000)!=WAIT_OBJECT_0) ExitProcess(97);
  if (loader && WaitForSingleObject(loader,5000)!=WAIT_OBJECT_0) ExitProcess(97);
  gesher_alias_gate_v1(NULL,NULL,NULL);
  if (loaded && (!FreeLibrary(loaded) || GetModuleHandleW(L"embedded-provider.dll"))) return 3;
  if (worker && !CloseHandle(worker)) ok=0; if (loader && !CloseHandle(loader)) ok=0;
  if (entered && !CloseHandle(entered)) ok=0; if (release && !CloseHandle(release)) ok=0; if (contended && !CloseHandle(contended)) ok=0;
  if (pthread_key_delete(first) || !GetProcessHandleCount(GetCurrentProcess(),&after) || after>before+8) return 4;
  if (!ok) return 2;
  printf("PASS probe static DLL admission blocked by active key publication handles=%lu/%lu\n",before,after); return 0;
}
