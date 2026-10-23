# 0014 — %fs for bionic's TLS in loaded Android libraries

**Status:** accepted, 2026-10-23 (native-libraries track)

## Context
An app's NDK `.so` is x86-64 Android code. Like all Android native code it is
System V ABI (Gesher already enters JNI and ART's stubs that way,
[0006](0006-managed-abi.md)), and it reaches thread-local state through the
**`%fs`** segment, the way bionic's dynamic linker sets it up on Linux
(variant II):

- `%fs:0` is the thread pointer (a self-pointer), the top of the thread's
  static TLS block; thread-local variables are at negative offsets from it
  (`R_X86_64_TPOFF64`), and general-dynamic access goes through
  `__tls_get_addr` with `R_X86_64_DTPMOD64`/`DTPOFF64`.
- `%fs:0x28` (bionic TCB slot 5) holds the **stack-protector cookie**; every
  function the NDK compiles with `-fstack-protector` reads it in its prologue
  and epilogue.

On Linux bionic points the `%fs` base at the block with `arch_prctl`. On
Windows `%fs` base is 0 for user code and the TEB lives in `%gs`; there is no
Win32 call to move `%fs`. This is the mirror of the `%gs` problem ART's
assembly had ([0006](0006-managed-abi.md)), but here the accesses are in the
app's prebuilt `.so`, not in source we can patch.

## Options considered
1. **Load-time patching of `%fs` accesses** in the `.so`'s `.text`, as Gesher
   rewrote ART's `%gs` loads. Here the code is prebuilt and the `0x64` (`%fs`)
   prefix sits on instructions of many lengths, so rewriting in place is
   invasive and fragile (it needs a disassembler and relocation of moved code).
2. **`WRFSBASE`/`RDFSBASE`.** x86-64 added these to let *user* code read and
   write the `%fs`/`%gs` base, gated on `CR4.FSGSBASE`. Windows 10 (1607+)
   enables `FSGSBASE` on every CPU that has it (Ivy Bridge and later), and the
   kernel then saves and restores the bases across context switches. If that
   holds on the runners, a single `WRFSBASE` per thread sets `%fs` to a
   bionic-shaped block and everything the `.so` does through `%fs` just works.

## Decision
Use **`WRFSBASE`**, and prove it on the CI runners before relying on it, the
way `probes/platform` validates ART's Windows primitives.

- `probes/elf/fsbase_probe.cc` checks, on both runner images, that
  `RDFSBASE`/`WRFSBASE` do not fault (so `CR4.FSGSBASE` is on), that a
  `%fs:`-relative load reads what was written, and — the real question —
  that the base **survives a context switch, a blocking wait and an APC**, and
  that each thread keeps its own. The track test (`tracks/40-native-libs.ps1`)
  fails if any of this does not hold.
- The loader builds one bionic variant-II block per thread
  (`compat/elf/tls.cc`): a static TLS area below the thread pointer with every
  loaded `PT_TLS` module's image, and the TCB slots above it (`%fs:0`
  self-pointer, `%fs:0x28` a fixed non-zero stack cookie). `SetupThreadTls()`
  points `%fs` at it with `WRFSBASE`; it runs on every thread that enters
  loaded code (the main thread, and bionic's `pthread_create` wrapper for
  threads the `.so` starts).
- Every load-time module is given **static (initial-exec) offsets**, so
  `R_X86_64_TPOFF64` and `__tls_get_addr` (general-dynamic) resolve against the
  same per-thread block; `__tls_get_addr` is a System V entry in the bionic
  shim that returns `thread_pointer + module.tp_offset + offset`.

Setting `%fs` does not disturb Gesher's own Windows code, which uses `%gs`
(the TEB) for its thread-locals and the CRT; the two segments are independent.

## Fallback
If a supported Windows/CPU combination ever reports `FSGSBASE` off, the probe
says so and the loader falls back to option 1 for that host (not implemented
while every target runner passes the probe). The static-TLS-only model also
means a `PT_TLS` module loaded *after* a thread started is not in that thread's
block; today every app `.so` and its dependencies load before any app thread
starts, and a growable per-thread DTV is left for when that stops being true.

## Consequences
- No disassembly or code patching of the app's `.so`; one `WRFSBASE` per
  thread.
- Depends on Windows preserving the user `%fs` base across scheduling, which
  the probe asserts on every CI run, so a regression (an older image, a
  different CPU) is caught as a test failure, not a silent corruption.
