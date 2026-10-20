// gesher-base: a per-thread pointer readable from assembly through %gs.
//
// On Linux, ART points the gs segment base at the current art::Thread. On
// Windows gs is the TEB, so Gesher keeps the pointer in a TLS slot instead
// and publishes the slot's TEB offset (see docs/decisions/0006-managed-abi.md):
//
//   movq gesher_thread_self_slot(%rip), %reg
//   movq %gs:(%reg), %reg
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TEB offset of the slot; valid once gesher_set_thread_register() has run.
extern uintptr_t gesher_thread_self_slot;

void gesher_set_thread_register(void* value);

static inline void* gesher_get_thread_register(void) {
  void* value;
  __asm__ __volatile__("movq %%gs:(%1), %0" : "=r"(value) : "r"(gesher_thread_self_slot));
  return value;
}

#ifdef __cplusplus
}
#endif
