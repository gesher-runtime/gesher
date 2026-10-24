// gesher-base: the per-thread pointer behind ART's thread register.
//
// The pointer (ART's Thread*) is in TLS slot 63 of the TEB, at
// %gs:GESHER_THREAD_SELF_TEB_OFFSET: compiled code (dex2oat's, the JIT's)
// reaches it with that constant, the runtime's assembly through
// gesher_thread_self_slot (docs/decisions/0033).

#include "gesher/thread_register.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

namespace {

// TEB64.TlsSlots: 64 inline slots. Indices from 64 up live in a separately
// allocated array and cannot be reached with a single %gs-relative load.
constexpr uintptr_t kTebTlsSlots = 0x1480;
constexpr DWORD kInlineTlsSlots = 64;
constexpr DWORD kFixedSlot = (GESHER_THREAD_SELF_TEB_OFFSET - kTebTlsSlots) / sizeof(void*);
static_assert(kFixedSlot == kInlineTlsSlots - 1, "the last inline TLS slot");

INIT_ONCE slot_once = INIT_ONCE_STATIC_INIT;
DWORD slot_index = TLS_OUT_OF_INDEXES;
bool slot_fixed = false;

BOOL CALLBACK AllocateSlot(PINIT_ONCE, PVOID, PVOID*) {
  // TlsAlloc hands out the lowest free index: take the ones below the fixed
  // slot until it comes, then give them back.
  DWORD taken[kInlineTlsSlots];
  DWORD count = 0;
  DWORD index;
  while ((index = TlsAlloc()) != TLS_OUT_OF_INDEXES && index < kFixedSlot) {
    taken[count++] = index;
  }
  DWORD first_free = 0;
  if (index == kFixedSlot) {
    slot_index = index;
    slot_fixed = true;
  } else {
    // Someone has slot 63: any inline slot still works for the runtime's
    // assembly, not for compiled code (the runtime then interprets only).
    if (index != TLS_OUT_OF_INDEXES) TlsFree(index);
    if (count > 0) {
      slot_index = taken[0];
      first_free = 1;
    }
  }
  for (DWORD i = first_free; i < count; ++i) TlsFree(taken[i]);
  if (slot_index == TLS_OUT_OF_INDEXES || slot_index >= kInlineTlsSlots) {
    fprintf(stderr, "gesher: no inline TLS slot for the thread register (got %lu)\n",
            static_cast<unsigned long>(slot_index));
    abort();
  }
  gesher_thread_self_slot = kTebTlsSlots + sizeof(void*) * slot_index;
  return TRUE;
}

void EnsureSlot() { InitOnceExecuteOnce(&slot_once, AllocateSlot, nullptr, nullptr); }

// As early as possible, while the low TLS indices are still free.
struct EarlySlot {
  EarlySlot() { EnsureSlot(); }
} early_slot;

}  // namespace

extern "C" {

uintptr_t gesher_thread_self_slot = 0;

void gesher_set_thread_register(void* value) {
  EnsureSlot();
  TlsSetValue(slot_index, value);
}

int gesher_thread_register_is_fixed(void) {
  EnsureSlot();
  return slot_fixed ? 1 : 0;
}

}  // extern "C"
