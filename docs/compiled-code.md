# Compiled code: nterp, AOT and the JIT

Until the codegen track, every Gesher process ran ART's switch interpreter
only (`-Xint`): the boot image had no code, the JIT was off and nterp (ART's
assembly interpreter) was disabled, because ART's x86-64 compiled code and
assembly reach the current `Thread` through `%gs`, which on Windows is the
TEB. This brings the three faster execution modes to Windows. The design is
[ADR 0033](decisions/0033-codegen-compiled-code-on-windows.md); it extends
[ADR 0006](decisions/0006-managed-abi.md).

## What changed
- **`Thread*` at a constant TEB offset.** gesher-base takes TLS slot 63 at
  startup, so `Thread*` is always at `%gs:0x1678`, which compiled code reads
  with one instruction (ADR 0033; `compat/posix/src/thread_register.cc`).
- **The x86-64 code generator** reaches every Thread field through that slot
  instead of `%gs:offset` (ART patch 0015), as do the oat trampolines and the
  math entrypoints (System V wrappers).
- **Faults become signals** and thread stacks grow for stack probes
  (`compat/posix/src/fault_signals.cc`): implicit null checks and the stack
  overflow probe that nterp and compiled code rely on now work.
- **nterp** is on (implicit null and stack overflow checks on, patch 0014).
- **The boot image** carries AOT-compiled code for its primary image
  (`--compiler-filter=speed` for core-oj, core-libart, core-icu4j); the
  extensions stay verify-only for now (`tools/boot-image/compile.ps1`).
- **The JIT** is linked into dalvikvm (libart-compiler) and compiles app code;
  the code cache uses one RWX mapping toggled with `mprotect` (ART's
  single-mapping path), since gesher-base has no executable file views for the
  dual W^X mapping yet.

## Diagnosis switches
- `GESHER_ART_INTERPRET_ONLY=1` — the switch interpreter only, explicit checks,
  no nterp, no compiled code, no JIT (the configuration before this work).
- `GESHER_ART_NO_AOT=1` — ignore the boot image's compiled code (nterp, JIT).
- `GESHER_LOG_EXECUTION=1` — log the execution mode and the boot image's
  compiled-method count.
- `GESHER_TRACE_SIGNALS=1` — log the first 200 faults and what became of each.

## Status
Verified in CI (`tests/windows/tracks/70-codegen.ps1`, run-tests.ps1 sets
`GESHER_REQUIRE_BOOT_IMAGE`), on windows-2022 and windows-2025:

- [x] **nterp** runs Hello, the stress test and the benchmark; it matches the
      switch interpreter's benchmark checksum.
- [x] **The JIT** compiles app code (its code cache fills) and matches the
      same checksum.
- [x] **AOT**: the boot image's primary image is compiled with `speed` (53,927
      methods with compiled code in CI); the nterp and JIT modes run app code on
      nterp / the JIT while the boot library (String, Math, collections, ...)
      runs its AOT code.
- [x] **Stress** (implicit null checks, exceptions through compiled frames,
      GC with compiled frames on the stack, threads and monitors, JNI from
      compiled code, proxies, reflection, array/arithmetic/cast exceptions)
      passes under the switch interpreter, nterp (over AOT) and the JIT (over
      AOT), and under the interpret-only diagnosis mode.
- [x] **ART's own run-tests** (a subset of `art/test` that needs no native
      library): 008-exceptions, 021-string2, 046-reflect, 080-oom-throw,
      083-compiler-regressions, 114-ParallelGC, 123-compiler-regressions-mt —
      the same stdout under the switch interpreter, nterp and the JIT.
      (003-omnibus-opcodes, 061-out-of-memory and
      201-built-in-except-detail-messages do not pass under the switch
      interpreter on Gesher either, for reasons unrelated to compiled code;
      they are reported, not counted.)

### Benchmark (`gesher.Bench`, best of 3 rounds, ms; windows-2025, run 40068699984)
A CPU-bound dex benchmark; app code under the switch interpreter vs nterp vs
the JIT (the boot library runs AOT code in the nterp and JIT columns):

| kernel | switch | nterp | JIT |
|---|---|---|---|
| fib | 20 | 4 | 0 |
| int-loops | 149 | 37 | 3 |
| sieve | 42 | 10 | 1 |
| sort | 150 | 20 | 7 |
| virtual-calls | 125 | 33 | 1 |
| allocation | 730 | 149 | 141 |
| strings | 163 | 6 | 6 |
| floats | 95 | 44 | 3 |
| **total** | **1474** | **303** | **162** |

nterp is about 5x the switch interpreter on this benchmark's total, the JIT
about 9x (the full process wall time, which includes startup, 4791 -> 1233 ->
864 ms). The checksum is identical in all three modes.

## Not done yet
- **Deep-recursion `StackOverflowError` under nterp and the JIT.** The stack
  overflow probe faults correctly, but ART's handler runs on the faulting
  thread's own near-exhausted stack (Windows vectored exception handlers have
  no equivalent of `sigaltstack`), so throwing the error overflows the stack
  for real (`STATUS_STACK_OVERFLOW`). The switch interpreter and
  interpret-only mode recover (bounded by the managed stack check); the
  `gesher.Stress` "so" check runs only there. An alternate signal stack is the
  fix.
- **Compiled JNI stubs.** Native methods use the generic JNI trampoline under
  AOT and the JIT; the JNI macro assembler still reaches `Thread*` through
  `%gs` (ADR 0033).
- **AOT for the extensions** (framework.jar, gesher-app.jar) and an app image.
- **Dual-mapped JIT code cache** (W^X); the single RWX mapping is used.
