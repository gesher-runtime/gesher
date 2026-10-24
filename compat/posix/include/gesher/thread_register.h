// gesher-base: a per-thread pointer readable from assembly through %gs.
//
// On Linux, ART points the gs segment base at the current art::Thread. On
// Windows gs is the TEB, so Gesher keeps the pointer in a TLS slot instead
// (docs/decisions/0006-managed-abi.md, 0033): TLS slot 63, whose TEB offset
// is a constant, so that compiled code reads it with one instruction:
//
//   movq %gs:GESHER_THREAD_SELF_TEB_OFFSET, %reg
//
// The runtime's assembly reads the slot's offset from gesher_thread_self_slot:
//
//   movq gesher_thread_self_slot(%rip), %reg
//   movq %gs:(%reg), %reg
#pragma once

// TEB64.TlsSlots[63].
#define GESHER_THREAD_SELF_TEB_OFFSET 0x1678

#ifndef __ASSEMBLER__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TEB offset of the slot; valid once gesher_set_thread_register() has run
// (gesher-base takes the slot at startup).
extern uintptr_t gesher_thread_self_slot;

void gesher_set_thread_register(void* value);

// 1 when the slot is TLS slot 63 (GESHER_THREAD_SELF_TEB_OFFSET), as compiled
// code requires; 0 when another module had taken it.
int gesher_thread_register_is_fixed(void);

static inline void* gesher_get_thread_register(void) {
  void* value;
  __asm__ __volatile__("movq %%gs:(%1), %0" : "=r"(value) : "r"(gesher_thread_self_slot));
  return value;
}

#ifdef __cplusplus
}
#endif

#endif  // __ASSEMBLER__
