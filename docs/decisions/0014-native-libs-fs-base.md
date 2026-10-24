# 0014 — %fs for bionic's TLS in loaded Android libraries

**Status:** accepted, 2026-10-23 (native-libraries track); the WRFSBASE decision was superseded the same day by the load-time rewrite at the end of this record

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

## Finding (2026-10-23) — WRFSBASE did not hold on the CI runners

The first probe run (CI `39942420552`, both `windows-2022` and `windows-2025`)
showed **`WRFSBASE` does not give a usable `%fs` on these runners**: the probe
took an access violation on a `%fs` memory load that followed a kernel
transition, i.e. `CR4.FSGSBASE` is on and `WRFSBASE` executes, but the base is
**not preserved** (Windows appears to reset the user `%fs` base, so the next
`%fs`-relative access reads near-null). The step-1 ELF loader still **passed**
on both runners, because `trivial.so` touches no `%fs`.

Consequence: the primary decision above **cannot be relied on as-is**. The
`fsbase` probe has been made crash-safe and now reports a precise verdict
(checking the base with `RDFSBASE`, which touches no memory), and the track
check is **informational** so the loader's step-1 result is not blocked by it.
Before any `%fs`-using `.so` runs (anything with `-fstack-protector` or
`thread_local`, which includes libc++_shared), the next session must either:

1. confirm, with the revised probe, whether re-asserting `WRFSBASE` right
   before entering `.so` code (and after every return from a Windows call that
   could have reset it) is enough — fragile, and racy against preemption; or
2. implement the **fallback**: patch the `.so`'s `%fs` accesses at load time
   (rewrite the `0x64` segment-prefixed instructions to reach the per-thread
   block another way), the way ART's `%gs` loads were handled
   ([0006](0006-managed-abi.md)); or
3. find another mechanism (e.g. a thread that keeps re-arming the base, or an
   OS/driver-assisted base set).

This is the track's main open problem; see `docs/native-libs.md`.

## Decision, revised (2026-10-23): rewrite `%fs` accesses at load time

This **replaces the WRFSBASE decision** above (option 2), which the runners
disproved, with option 1 done in a way that needs no code relocation. The
loader no longer executes `WRFSBASE`; the `fsbase` probe stays as an
informational CI line.

- **Where the rewrite happens.** `compat/elf/fs_rewrite.cc`, called from
  `LoadCommon` right after the segments are mapped (still read-write) and
  before relocation, because IFUNC resolvers run during relocation and may be
  stack-protected. It never touches anything a relocation writes (the
  displacements it changes are the constants 0 and 0x28), and `.text` gets
  its final read-execute protection afterwards as before.
- **Finding the instructions.** A small x86-64 length decoder
  (`compat/elf/x86_decode.cc`: legacy prefixes, REX, the one-byte/0F/0F38/0F3A
  maps, VEX, EVEX, XOP, 3DNow!) walks the code instruction by instruction over
  each FDE's `[pc_begin, pc_begin + range)` from `.eh_frame_hdr` (every NDK
  function has unwind info), else over `SHF_EXECINSTR` sections, else over the
  executable segments. Decoding from known function starts means a `0x64`
  byte inside another instruction or in data is never taken for a prefix.
  The decoder agreed with `objdump` on every one of 7.2 M instructions of a
  GCC-built binary, and on an AVX-512 OpenBLAS (only `fwait`-merged x87
  mnemonics and data in `.text` differed).
- **What each form becomes** (instruction lengths unchanged):
  - `%fs:0x28` (the stack guard) → `%gs:0x30`, `NT_TIB.Self`: per-thread,
    constant for the thread's life, non-zero, and valid on **every** thread,
    including ones the loader never set up (ART's, the CRT's). Prologue and
    epilogue read the same slot, so any stable value is a correct cookie; it is
    not secret (the TEB address), an accepted loss of hardening. The TCB's own
    slot 5 holds the same value for code that reaches it through the thread
    pointer.
  - `%fs:0` (the thread pointer, initial-exec address computations) →
    `%gs:[0x1480 + 8*i]`, a `TlsAlloc` slot (`i < 64`, the same constraint and
    mechanism as ART's thread register, [0006](0006-managed-abi.md)) holding
    the thread's bionic block pointer. Only the `0x64` → `0x65` prefix byte and
    the disp32 change.
  - `%fs:(%reg)` / `%fs:d(%base,%index)` (initial-exec loads and stores) cannot
    be rewritten in place. The instruction is replayed in an out-of-line stub:
    `lea -128(%rsp),%rsp; pushfq; add %gs:slot,%base; popfq; lea 128(%rsp),%rsp`,
    the instruction without its `%fs` prefix, the same sequence with `sub`
    (skipped when the instruction overwrites the base register), and a jump
    back. The red zone and the flags are preserved. The site becomes
    `jmp rel32` to the stub when the instruction is at least 5 bytes and the
    stub (allocated next to the object) is within ±2 GiB; otherwise an `int3`
    that a vectored handler redirects to the stub. Supported: the integer
    `mov`/ALU/`cmp`/`test`/`movzx`/`movsx`/`movsxd`/`imul`/`cmov`/`xchg`/
    `inc`/`dec`/`not`/`neg` forms with a register or immediate operand.
  - Anything else (another `%fs` displacement, i.e. a bionic TCB slot Gesher
    does not provide; a RIP-relative `%fs` operand; an instruction the stub
    cannot replay, such as VEX or a partial write of its own base register)
    becomes an `int3`; the load prints the site, and reaching it prints the
    library+offset, the reason and the original bytes, then aborts. The load
    itself does not fail, so a library whose odd path is never taken still
    works.
- **Diagnostics for misses.** The same vectored handler names an access
  violation inside loaded code that comes from an un-rewritten `%fs`
  instruction, or from a `%gs`-slot access on a thread with no TLS block
  (`SetupThreadTls` not called), then lets the fault proceed.
- **Thread setup.** `SetupThreadTls()` (bionic `pthread_create`'s threads, and
  any thread that enters loaded code with initial-exec TLS; the JNI entry path
  of step 4 is the next caller) builds the block and stores its thread pointer
  in the slot. The stack guard and `__tls_get_addr` need no setup.
- **Static TLS with a surplus; late modules.** Each thread's block reserves
  1 MiB of address space below the thread pointer and commits pages as
  modules are assigned. A `PT_TLS` module loaded after threads exist gets the
  next static offset; the space is committed in every live thread's block and,
  once the module is relocated, its image is copied into each of them (its code
  cannot have run yet). A module that does not fit is **dynamic-only**:
  `__tls_get_addr` gives each thread its own copy on first use, and an
  initial-exec relocation against it fails the load with a message. Blocks
  are freed at thread exit (an FLS callback). `__tls_get_addr` caches each
  module's base per thread, so the general-dynamic path takes no lock after
  the first call.

### Consequences of the revision
- No dependence on an undocumented kernel behaviour; everything used (`%gs`
  as the TEB, `TlsAlloc` slots, `NT_TIB.Self`) is stable Windows ABI.
- Load time grows by one linear decode of the object's code (tens of MB/s;
  libraries are small); runtime cost is nil for the guard and thread pointer,
  a few instructions per register-based initial-exec access, and an exception
  round trip only for short (< 5 byte) ones whose stub is out of `rel32`
  range.
- Code that computes `%fs`-relative addresses at run time (JIT-generated code
  inside the app, or self-modifying code) is not covered; it would fault and be
  named by the diagnostic handler.
