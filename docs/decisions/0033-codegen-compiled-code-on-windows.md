# 0033 — Compiled code on Windows: nterp, AOT and the JIT

**Status:** accepted, 2026-10-24 (codegen track). Extends ADR 0006.

## Context
Until now every Gesher process ran ART's switch interpreter only (`-Xint`):
the boot image had no code (`--compiler-filter=verify`, ADR 0031), the JIT
was not linked and nterp, ART's assembly interpreter, was off. Three things
kept compiled code out:

1. **The thread register.** ART's x86-64 code generator, its trampolines and
   its JNI stubs reach the current `Thread` as `%gs:offset`. On Windows `%gs`
   is the TEB; ADR 0006 put `Thread*` in a TLS slot whose TEB offset the
   runtime's assembly reads from a global (`gesher_thread_self_slot`). Code
   that dex2oat writes into an oat file, or the JIT into its code cache, cannot
   reach that global with a RIP-relative load, and the slot's index was
   whatever `TlsAlloc` returned in that process.
2. **Implicit checks.** nterp and compiled code dereference null references
   and let the fault become a `NullPointerException`, and probe the stack
   (`testq %rax, -gap(%rsp)` at each method's entry) so that a fault in ART's
   protected pages at the bottom of the stack becomes a `StackOverflowError`.
   x86-64 compiled code has no explicit stack overflow check at all. Windows
   delivers faults as exceptions, not signals, and grows a thread's stack page
   by page through a guard page, which a probe 8 KiB down skips.
3. **The ABI at the remaining boundaries.** Managed code calls the C library's
   math functions through the entrypoint table (`pCos`, `pPow`, ...): those
   are Microsoft ABI on Windows (shadow space, other callee-saved registers).

## Decision
- **`Thread*` in TLS slot 63, a constant.** gesher-base takes TLS index 63 at
  startup (it takes the lower free indices until `TlsAlloc` hands out 63,
  then frees them), so `Thread*` is always at `%gs:0x1678`
  (`GESHER_THREAD_SELF_TEB_OFFSET`, `TEB.TlsSlots[63]`). Compiled code reads
  it with one instruction; the runtime's assembly keeps reading the slot
  through `gesher_thread_self_slot` (same value). If another module had taken
  slot 63 before gesher-base ran, the runtime falls back to interpreting only
  and says so.
- **The code generator** (ART patch 0015) replaces each `gs()->op(Thread
  field)` by a load of `Thread*` into a scratch register and an ordinary
  memory operand, per site:
  - `TMP` (R11), the code generator's own scratch register, where it is free:
    runtime calls (R11 is never an argument), suspend checks, exception
    loads and clears, card marking (into the card register itself), the
    string-init entrypoint (into its temp), intrinsics;
  - RAX for `art_quick_update_inline_cache`, which takes the inline cache in
    R11 and may clobber RAX;
  - where nothing is free (the class-initialization check at frame entry,
    where TMP holds the initializing thread id; marking checks in type
    checks and GC root loads), RAX is saved 16 bytes below the stack pointer
    and restored; LEA and MOV leave the flags of the access alone;
  - the read barrier mark entrypoints (`pReadBarrierMarkRegNN`) take the
    reference in any register, R11 included, and preserve all others: they
    are called through a slot 16 bytes below the stack pointer. They do not
    walk the stack, which is the only thing a lower stack pointer would
    disturb.
  The trampolines dex2oat writes into oat files jump through R11 (the
  hidden arguments of the IMT conflict and critical native paths are in
  RAX). Patch 0012's re-pointing of the boot image's stubs at libart's stays
  (same code; it is also where the runtime counts the image's compiled methods).
- **No compiled JNI stubs yet.** The JNI compiler's macro assembler has 17
  Thread accesses with the scratch register often busy; native methods keep
  the generic JNI trampoline (assembly, ported) under AOT and the JIT. This is
  the one place compiled code still differs from Linux.
- **Faults as signals, stacks grown for probes** (gesher-base,
  `compat/posix/src/fault_signals.cc`). A vectored exception handler, first
  in the chain:
  - commits the pages down to an access violation's address when it is in the
    current thread's stack reservation below the committed part and only
    reserved, puts a guard page below them, moves `TEB.StackLimit` and
    retries the instruction;
  - otherwise delivers access violations as `SIGSEGV` (illegal instruction as
    `SIGILL`, integer divide faults as `SIGFPE`) to the `sigaction` handler,
    libsigchain's in ART processes, with a Linux `siginfo_t` and a `ucontext_t`
    made from the `CONTEXT` (general registers, flags, the FXSAVE area), and
    continues with what the handler left in the ucontext, as after
    `sigreturn`. The signals' default action is a gesher-base handler that
    records that nobody took the fault: the exception then continues to
    Windows' own handling (SEH `__try` in native code, the crash reporter).
  `mprotect` of reserved stack pages commits them with that protection, so
  ART's protected region at the bottom of each stack works as on Linux. ART
  skips its `madvise` over the rest of the stack (patch 0014).
- **A 32 KiB stack overflow gap** on x86-64 (`ART_STACK_OVERFLOW_GAP_x86_64`,
  8 KiB on Linux): there is no `sigaltstack`, the fault of a stack probe is
  handled on the thread's own stack below the probing frame (exception record,
  `CONTEXT` with its extended state, the ucontext, ART's handler), and the gap
  is what is left there.
- **Implicit null and stack overflow checks on**, suspend checks explicit as
  on x86-64 Linux (patch 0014). dex2oat and the JIT compile with the same.
- **System V wrappers for the math entrypoints** (`gesher_sysv_cos`, ...,
  patch 0015), as ADR 0006 foresaw.
- **membarrier** is `FlushProcessWriteBuffers` (process-wide barrier; x86-64
  needs no instruction cache maintenance for code the JIT writes).
- **The JIT**: dalvikvm links libart-compiler (the JIT's `jit_create`).
  `memfd_create` is not ART's on Windows, so the code cache takes ART's
  single-mapping path (one RWX mapping toggled with `mprotect`), which ART
  supports when dual mapping is unavailable. Dual mapping (W^X with two views)
  needs executable file views, which gesher-base's `mmap` does not make yet.
- **Diagnosis switches.** `GESHER_ART_INTERPRET_ONLY=1` restores the old
  configuration exactly (switch interpreter, explicit checks, no JIT, no
  compiled code); `GESHER_ART_NO_AOT=1` ignores oat code (nterp and the JIT
  only); `GESHER_LOG_EXECUTION=1` logs the execution mode and the boot
  image's compiled method count.

## Consequences
- The boot image carries compiled code for the primary image (core-oj,
  core-libart, core-icu4j: `--compiler-filter=speed`); app code runs on nterp
  and, with the JIT, compiled. See docs/compiled-code.md for what CI measures.
- Every Gesher process takes TLS slot 63, and handles every access violation
  first (a few comparisons for faults that are not its business).
- JNI calls from compiled code go through the generic JNI trampoline, slower
  than compiled stubs; porting the JNI macro assembler is the next step.
- A thread's stack is committed on demand by gesher-base below Windows' own
  guard page mechanism: `TEB.StackLimit` follows, so SEH and `__chkstk` in
  native code see a consistent stack.
