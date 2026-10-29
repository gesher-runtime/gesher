/* Actual unloadable callback module for CI-only generation lifetime probe. */
#include <windows.h>
typedef struct { HANDLE entered, release; volatile LONG calls, failed; } callback_state;
__declspec(dllexport) void __cdecl probe_destructor(void *value) {
  callback_state *state=value;
  InterlockedIncrement(&state->calls);
  if (!SetEvent(state->entered) || WaitForSingleObject(state->release,5000)!=WAIT_OBJECT_0)
    InterlockedExchange(&state->failed,1);
}
