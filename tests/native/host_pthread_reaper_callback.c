/* Probe observer only: DLL detach does no allocation, waits or provider calls. */
#include <windows.h>
typedef struct { HANDLE entered,release; volatile LONG calls,failed; } callback_state;
static volatile LONG *detach_tid;
__declspec(dllexport) void __cdecl probe_observe_detach(volatile LONG *tid) { detach_tid=tid; }
__declspec(dllexport) void __cdecl probe_destructor(void *value) {
  callback_state *state=value;
  InterlockedIncrement(&state->calls);
  if (!SetEvent(state->entered) || WaitForSingleObject(state->release,5000)!=WAIT_OBJECT_0)
    InterlockedExchange(&state->failed,1);
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason==DLL_PROCESS_DETACH && detach_tid)
    InterlockedExchange(detach_tid,(LONG)GetCurrentThreadId());
  return TRUE;
}
