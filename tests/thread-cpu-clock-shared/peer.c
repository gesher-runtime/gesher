/* A separately compiled image; every pthread and clock symbol imports the
   same provider DLL. No Gesher header forwarding or private clock API. */
#include "peer.h"
#include <windows.h>

static DWORD exit_gate_slot = TLS_OUT_OF_INDEXES;
static struct clock_peer_api api;

static int arm_exit_gate(struct clock_exit_gate* gate) {
  return TlsSetValue(exit_gate_slot, gate) ? 0 : -1;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    exit_gate_slot = TlsAlloc();
    if (exit_gate_slot == TLS_OUT_OF_INDEXES) return FALSE;
    /* dllimport function addresses are runtime IAT values, not C constant
       initializers. Publish this immutable table after loader resolution. */
    api = (struct clock_peer_api){sizeof(api), pthread_self, pthread_create,
        pthread_join, pthread_getcpuclockid, clock_gettime, clock_getres,
        arm_exit_gate};
  } else if (reason == DLL_THREAD_DETACH) {
    struct clock_exit_gate* gate = TlsGetValue(exit_gate_slot);
    if (gate) {
      TlsSetValue(exit_gate_slot, NULL);
      /* Isolated fixture boundary: this later-loaded DLL runs before its
         supplier dependency's final detach callback. Every participating
         thread/event already exists; the controller creates no thread or
         module while the gate holds the loader lock. */
      if (!SetEvent(gate->reached)) InterlockedExchange(&gate->error, 1);
      if (WaitForSingleObject(gate->release, 30000) != WAIT_OBJECT_0)
        InterlockedExchange(&gate->error, 2);
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    TlsFree(exit_gate_slot);
  }
  return TRUE;
}

__declspec(dllexport) const struct clock_peer_api* clock_peer_v1(void) {
  return &api;
}
