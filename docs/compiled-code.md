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
- **The boot image** carries AOT-compiled code for its primary image and its
  extensions (`--compiler-filter=speed` for core-oj, core-libart, core-icu4j,
  framework.jar and gesher-app.jar; `tools/boot-image/compile.ps1`,
  `boot-image.yml`).
- **The JIT** is linked into dalvikvm (libart-compiler) and compiles app code;
  its code cache is dual-mapped (W^X): code is written through a read-write
  view of a memfd and runs from a separate read-execute view (ADR 0036).
- **Deep recursion** throws `StackOverflowError` under every execution mode:
  Windows' `EXCEPTION_STACK_OVERFLOW` at a stack probe is delivered as the
  `SIGSEGV` ART expects, and ART's protected region sits right above the
  stack guarantee (ADR 0036; `fault_signals.cc`, `pthread_attr_getguardsize`).
- **Compiled JNI stubs**: the JNI macro assembler and the JNI trampolines reach
  `Thread*` through the TLS slot (ART patch 0016); the boot image has AOT JNI
  stubs, the JIT compiles the others.
- **Oat files are mapped, not copied**: gesher-base's `mmap` fills placeholder
  reservations with views (`MapViewOfFile3`), executable ones included, so an
  oat file's code is a view of the file shared between processes (ADR 0036).

## Diagnosis switches
- `GESHER_ART_INTERPRET_ONLY=1` — the switch interpreter only, explicit checks,
  no nterp, no compiled code, no JIT (the configuration before this work).
- `GESHER_ART_NO_AOT=1` — ignore the boot image's compiled code (nterp, JIT).
- `GESHER_LOG_EXECUTION=1` — log the execution mode and the boot image's
  compiled-method count.
- `GESHER_TRACE_SIGNALS=1` — log the first 200 faults and what became of each.
- `GESHER_TRACE_MMAP=1` — log every `mmap` call, and for file mappings whether
  they became a view (`an executable view of ...`) or a copy.
- `GESHER_MMAP_NO_PLACEHOLDERS=1` — plain allocations and views, oat code
  copied, no dual-mapped JIT cache (the configuration before ADR 0036).

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

## Status and next steps (stack track, ADR 0036)
Verified in CI on windows-2022 and windows-2025 plus package and e2e (ci run
40134720336, 23bd046; again in 40138006773):
- [x] Deep recursion: `StackOverflowError` caught and recovered from under
      switch, interpret-only, nterp and the JIT (app, boot AOT and native-call
      recursion, 4 rounds, main thread, new threads, two at once).
- [x] Compiled JNI stubs: 844 AOT JNI stubs in the boot image; the JIT
      compiles app natives' stubs (`gesher.JniStubs`).
- [x] Placeholder mappings and executable oat views (`mman_probe`, 26
      checks; 60-boot-image: oat code mapped as views, none copied).
- [x] Dual-mapped (W^X) JIT code cache.
- [x] Math entrypoints against StrictMath in the stress test, every mode.

Verified in ci run 40138006773 (1c158b4, both runners, package, e2e):
- [x] AOT for the extensions (boot-image.yml `-ExtensionFilter speed`):
      298,365 methods with compiled code with framework.jar and gesher-app.jar
      on the boot class path (2,581 JNI stubs), 54,771 with the primary image
      alone; the oat code is 5 executable views, 74.9 MB, none copied.
      dex2oat: primary 2.7 s, framework.jar 8.8 s, gesher-app.jar 0.7 s;
      boot-framework.oat is 75.7 MB.
- [x] Without placeholder mappings (`GESHER_MMAP_NO_PLACEHOLDERS=1`) the JIT
      takes its single mapping (patch 0017's `memfd_create` fails with
      ENOSYS) and the stress test passes.
- Startup with the compiled extensions (60-boot-image, median of 3):
  lifecycle.apk to `Activity.onCreate` 312 / 340 ms with the image, 744 /
  745 ms imageless (windows-2022 / 2025); before, with verify-only
  extensions (run 40134720336, windows-2022), 254 ms vs 541 ms imageless.
  Those runners were faster overall (benchmark totals 871 vs 1350 ms under
  the switch interpreter), so the ratio to imageless is the fairer
  comparison: 0.47 before, 0.42 / 0.46 now - no measurable startup cost from
  the larger oat files, which are mapped, not read.

Next (not part of this track's goal): an app image for the app's own
classes; a JIT profile / `speed-profile` for framework.jar to shrink its
75 MB oat file.
