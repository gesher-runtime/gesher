// gesher-base: memory fences, thread barriers, and instruction barriers.
// Provides portable primitives across x86-64 and aarch64.
#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Full memory fence / barrier
// On aarch64: dmb ish (Data Memory Barrier, Inner Shareable)
// On x86-64: mfence (or MemoryBarrier)
static inline void gesher_memory_barrier(void) {
#if defined(__x86_64__)
  __asm__ __volatile__("mfence" ::: "memory");
#elif defined(__aarch64__)
  __asm__ __volatile__("dmb ish" ::: "memory");
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// Thread barrier / spin-loop yield hint
// On aarch64: yield instruction
// On x86-64: pause instruction
static inline void gesher_thread_yield(void) {
#if defined(__x86_64__)
  __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  #if defined(_WIN32)
  SwitchToThread();
  #endif
#endif
}

// Instruction synchronization barrier
// On aarch64: isb (Instruction Synchronization Barrier)
// On x86-64: serializing instruction
static inline void gesher_instruction_barrier(void) {
#if defined(__aarch64__)
  __asm__ __volatile__("isb" ::: "memory");
#elif defined(__x86_64__)
  __asm__ __volatile__("cpuid" : : "a"(0) : "ebx", "ecx", "edx", "memory");
#endif
}

#ifdef __cplusplus
}
#endif
