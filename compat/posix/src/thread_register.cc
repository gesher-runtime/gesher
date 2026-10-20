// gesher-base: the per-thread pointer behind ART's thread register.

#include "gesher/thread_register.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

namespace {

// TEB64.TlsSlots: 64 inline slots. Indices from 64 up live in a separately
// allocated array and cannot be reached with a single %gs-relative load.
constexpr uintptr_t kTebTlsSlots = 0x1480;
constexpr DWORD kInlineTlsSlots = 64;

INIT_ONCE slot_once = INIT_ONCE_STATIC_INIT;
DWORD slot_index = TLS_OUT_OF_INDEXES;

BOOL CALLBACK AllocateSlot(PINIT_ONCE, PVOID, PVOID*) {
  slot_index = TlsAlloc();
  if (slot_index == TLS_OUT_OF_INDEXES || slot_index >= kInlineTlsSlots) {
    fprintf(stderr, "gesher: no inline TLS slot for the thread register (got %lu)\n",
            static_cast<unsigned long>(slot_index));
    abort();
  }
  gesher_thread_self_slot = kTebTlsSlots + sizeof(void*) * slot_index;
  return TRUE;
}

}  // namespace

extern "C" {

uintptr_t gesher_thread_self_slot = 0;

void gesher_set_thread_register(void* value) {
  InitOnceExecuteOnce(&slot_once, AllocateSlot, nullptr, nullptr);
  TlsSetValue(slot_index, value);
}

}  // extern "C"
