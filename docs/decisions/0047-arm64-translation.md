# 0047 — arm64 translation: dynarmic, in a proxy DLL, guest sharing the host address space

**Status:** accepted, 2026-10-25 (arm64 track)

## Context
Gesher runs x86-64 Android `.so` files natively on x86-64 Windows: the ELF
loader maps them and the bionic shim, libandroid, EGL/GLES and the JNI bridge
provide the host side (Phase 6, docs/native-libs.md). Many apps on F-Droid and
Play ship **only** `lib/arm64-v8a`. Phase 7's goal is that an arm64-only APK
runs exactly as an x86-64 one does: its native libraries load, JNI works both
ways, the NDK APIs Gesher already provides work, and a real arm64-only app runs
in the e2e job.

Windows on x86-64 cannot run AArch64 code, so the guest code must be
**translated**. Android itself does this with a *native bridge* (Houdini on
Intel devices, and AOSP's own Berberis): guest code is translated, and every
call across the ABI boundary goes through generated trampolines
("proxy libraries") that marshal AAPCS64 to the host ABI and back. ART's
`libnativebridge` is the hook — the class loader asks the bridge to load a
library of a foreign ISA.

### The translator — candidates evaluated
- **Berberis** (`frameworks/libs/binary_translation`, pinned
  android-16.0.0_r4). Apache 2.0, the native bridge Gesher would ideally
  reuse. But at the pinned tag its **only implemented guest is riscv64**: the
  `interpreter/`, `lite_translator/` (`riscv64_to_x86_64/`) and
  `heavy_optimizer/` trees are riscv64-only; `guest_state/arm64/` and
  `guest_abi/arm64/` exist but there is no arm64 decoder, interpreter or
  translator. Berberis would have to be written, not reused. Rejected for now.
- **QEMU TCG** user mode. A complete, mature aarch64 translator, but GPLv2.
  ADR 0001 forbids copying from GPL projects and requires Gesher stay Apache
  2.0 so our changes sit in the same files as AOSP's. Linking QEMU would make
  the product GPL. Rejected on license grounds.
- **dynarmic** (MerryMage; mirror `yuzu-mirror/dynarmic`, pinned commit
  `9d4582339990d4eae53f1dc7160686920fc2075c`). **0BSD** — compatible with
  ADR 0001 (permissive, attribution-optional). A mature AArch64 (A64)
  **frontend** and an **x86-64 backend** — exactly Gesher's host and guest.
  Already builds on Windows (its code cache and exception handler have
  `_WIN32` paths) and registers unwind info with `RtlAddFunctionTable`. Its
  externals (fmt, mcl, xbyak, Zydis, robin-map; Boost headers) are permissive.
  **Chosen.**

### The execution model
Berberis and Houdini give the guest its own address space and copy data across
the bridge. Gesher does **not**: the guest `.so` is mapped into the host
process like an x86-64 one, and **the guest's virtual addresses are real host
addresses**. dynarmic supports this directly — its memory callbacks (or a
`fastmem_pointer` of 0 with a 64-bit address space, so the generated access is
`[0 + vaddr]`) read and write host memory at the guest address with no copy.
So guest and host share pointers: a string the guest passes to `strlen` is the
host's own memory, an object the guest hands to the JNI bridge is ART's.

This makes the two directions of the bridge:

- **Guest → host.** A guest import is bound to a tiny **guest trampoline** in a
  code region of its own — `SVC #n; RET`, one per host function, `n` indexing
  the trampoline table (`SVC #0` stays a raw Linux syscall made by guest code,
  translated like `syscall(2)`; `SVC #0xFFFF` is the return stub). When the
  guest calls the import it runs the trampoline; dynarmic's `CallSVC(n)` maps
  the guest's X0–X7/V0–V7 and stack arguments to System V (the host's JNI,
  bionic and GLES entries are System V, ADR 0006/0017), calls the host
  function, and writes the result back to X0/X1/V0, and the `RET` returns to
  the guest caller. A host function with a `va_list` parameter (the shim's
  `v*printf`/`v*scanf`, `__android_log_vprint`, the JNI bridge's
  `Call*MethodV`) gets its AAPCS64 `va_list` converted to a System V one.
  A guest may also get a host function as a plain pointer (`dlsym`,
  `eglGetProcAddress`, a host JNIEnv table): dynarmic only translates code in
  registered guest regions, so a branch there raises a no-execute fault, and
  if the target is host code the translator calls it the same way and returns
  to the guest's X30.
- **Host → guest.** Host code calls a guest function — init/fini arrays,
  IFUNC resolvers, `JNI_OnLoad` and the `Java_*` natives through the JNI
  bridge, pthread start routines, qsort comparators, the NativeActivity
  callbacks — through an explicit **`host_entry(fn)` System V x86-64 stub** the
  loader obtains for each such function and hands to host code in the guest
  function's place. The stub (`gesher_guest_entry_shim`) spills the System V
  argument registers and enters the guest on the calling thread with a
  per-thread `dynarmic::A64::Jit`: the System V argument registers (and
  XMM0–7) become AAPCS64 X0–X7/V0–V7, the guest runs on its own stack until it
  returns to a sentinel X30 (a one-instruction return stub whose `SVC` halts
  the Jit), and X0/V0 become RAX/XMM0 for the host caller. The guest's
  executable pages are mapped **readable but not executable**, so a host call
  straight to a guest function pointer (a qsort comparator, a pthread start
  routine, a key destructor, NativeActivity's callbacks — anything the guest
  hands a host API) faults with DEP at the guest address; the translator's
  vectored exception handler, first in the chain, resumes the thread at that
  function's `host_entry` stub, which is then exactly a call of the stub.

The translator lives in a **proxy DLL, `gesher_arm64.dll`** (one export,
`gesher_arm64_init`, `compat/arm64/include/gesher/arm64_translator.h`), loaded
at run time only when an arm64 object must be loaded. dynarmic (and its
externals and Boost headers) therefore stay out of dalvikvm's build graph
entirely: the DLL is built by its own reusable workflow
(`.github/workflows/arm64.yml`) and shipped as an artifact, exactly as ANGLE's
DLLs are (ADR 0018). dalvikvm and the ELF loader link none of it; they
`LoadLibrary` the DLL through `compat/arm64` when they first meet an
`EM_AARCH64` object.

### The ELF loader and ABI selection
`compat/elf` gains EM_AARCH64 support additively: the AArch64 relocations
(`R_AARCH64_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS64`, `RELR`, `IRELATIVE`
and the TLS relocations `TLS_TPREL`, `TLSDESC`, `TLS_DTPMOD`, `TLS_DTPREL`),
AArch64 TLS (variant I: the thread pointer `TPIDR_EL0` points *below* the TCB,
thread-locals at positive offsets), and mapping executable segments
non-executable. x86-64 objects are unchanged — the loader branches on
`e_machine`.

`PackageManager`/the installer pick `arm64-v8a` when an APK ships no x86_64
libraries, and `Build.SUPPORTED_ABIS` lists `x86_64` then `arm64-v8a`, with
`ro.dalvik.vm.native.bridge` set as Android does.

## Decision
Translate guest AArch64 with **dynarmic** (A64 frontend, x86-64 backend, 0BSD),
in a proxy DLL `gesher_arm64.dll`, with the guest sharing the host address
space and trampolines marshalling AAPCS64 ⇄ the host ABI in both directions as
described above. dynarmic is pinned at
`yuzu-mirror/dynarmic@9d4582339990d4eae53f1dc7160686920fc2075c`.

### Guest-specific libc
bionic's LP64 types are the same on x86-64 and arm64 for nearly everything the
shim implements, so the guest calls the shim directly. The exceptions get
guest definitions in the DLL (`guest_libc.cc`, which the loader consults
before binding an import to the shim): `struct stat` (144 bytes on x86-64, 128
on arm64) and `struct epoll_event` (packed on x86-64) are converted;
`syscall(2)` and raw `SVC #0` map arm64's generic syscall numbers onto the
shim; `getauxval(AT_HWCAP)` reports what the translator implements (FP, ASIMD,
AES, PMULL, SHA1, SHA2, CRC32 — not LSE atomics, so outline-atomics code takes
the LL/SC path); `setjmp`/`longjmp` are AArch64 code emitted by the DLL; `dlsym`
returns these definitions. IFUNC resolvers get bionic's arm64 arguments.

### Atomics, TLS, faults
- Guest exclusive stores (`STXR`, what `std::atomic` and outline atomics
  compile to without LSE) go through dynarmic's global exclusive monitor to a
  host compare-and-swap at the guest address, so they are atomic against every
  thread. (dynarmic's default `MemoryWriteExclusive*` callbacks fail every
  store; an LL/SC loop then never ends.)
- TLS is variant I: `TPIDR_EL0` points at an 8-slot TCB (bionic's: the stack
  protector's guard is slot 5, `[TPIDR_EL0 + 40]`), then the static TLS of
  every guest module, at offsets from 64. `TLS_TPREL` and `TLSDESC` (with a
  guest resolver returning the descriptor's offset) are supported; every guest
  thread has its block, initialised from the modules' images.
- A fault while a thread runs guest code (a guest memory access, an undefined
  instruction, `BRK`, a branch to no code, a guest stack overflow onto the
  guard page) ends the process with an Android-style report — `Fatal signal 11
  (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0`, the cause, an approximate
  guest backtrace with object and symbol names — and exit status 128 + signal.

## Consequences
- 0BSD and Apache 2.0 do not conflict; dynarmic's notice is shipped with the
  DLL (`licenses/DYNARMIC-NOTICES.txt`), as ANGLE's is.
- The guest runs at translated-code speed, not native; a cache of translated
  blocks (dynarmic's own) keeps a hot path fast. Overhead is measured in
  docs/arm64.md.
- Sharing the address space removes all data copying across the bridge and lets
  the existing host side (shim, libandroid, EGL/GLES, JNI) be reused unchanged
  — the guest calls the very same System V entry points an x86-64 `.so` does.
- Faults *inside* guest code are the translator's to interpret (a guest null
  dereference is a host access violation at a fastmem/callback site); turning
  them into Android-like crashes rather than hangs is handled in the DLL.
- **Variadic calls and `va_list`.** A guest variadic call (`printf`,
  `CallIntMethod`) is marshalled by the same register/stack mapping as a fixed
  call: X0–X7 and V0–V7 to the System V argument registers and a window of
  guest stack words to the System V stack, which matches how System V reads
  them for up to six integer and eight floating-point arguments plus stack
  ones. A `va_list` argument is converted (remaining GP/FP save-area slots and
  the stack area); this is exact unless more than eight floating-point
  arguments precede integers that overflowed, or for `long double` (128-bit on
  arm64). Structs passed by value are marshalled as their integer words: an
  HFA (a struct of floats) is not (none of the shim's or the NDK's C APIs take
  one).
- Each nesting level of a guest→host→guest chain, and each guest thread, uses
  its own `dynarmic::A64::Jit` with its own translated-code cache (Run() is not
  reentrant and a Jit is single-threaded): 32 MiB for the outermost, 8 MiB per
  nested level, so deep callbacks and many threads cost memory. A shared block
  cache is possible future work. The nested guest runs below the outer guest's
  live frames on the thread's guest stack (8 MiB, guard page at the bottom).
- The thunk into System V host code saves `rsi`, `rdi` and `xmm6`–`xmm15`,
  which Microsoft x64 (the DLL and dynarmic's generated code) preserves across
  calls and System V does not.
- Not done: guest code that generates code at run time (a guest JIT: its
  pages are not registered guest code); guest signal handlers see the host's
  `ucontext`; `HWCAP_ATOMICS` code (LSE) is not translated by this dynarmic.
- Berberis remains the better long-term native bridge if its arm64 guest is
  completed upstream; the `gesher_arm64_init` interface would let it replace
  dynarmic behind the same DLL.

## Revision (2026-10-25, review by the successor arm64 agent)
A review of the first implementation corrected:
- guest store-exclusives always failed (dynarmic's default
  `MemoryWriteExclusive*`), so any guest atomic spun forever — the hang of ci
  40196210581; now a host CAS behind the global monitor;
- a guest fault, undefined instruction or a stray branch re-ran the same block
  up to a million times (a hang); now an Android-style fatal-signal report;
- the DEP redirect ran the whole guest call inside the vectored exception
  handler (a pthread start routine's entire thread ran there); now it resumes
  the thread at the function's `host_entry` stub;
- the System V thunk did not preserve `xmm6`–`xmm15` for its Microsoft x64
  caller (dynarmic keeps guest vector state there);
- `host_entry` dropped System V stack arguments 7+ (X6/X7 were zero);
- the TLS block put module TLS over bionic's TCB slots (the stack guard);
  TLS_TPREL against another module's symbol used the wrong module;
  TLSDESC was not handled; a dead thread's TLS block stayed in the publish list;
- the JNI bridge's sticky per-thread "guest mode" handed guest trampoline
  tables to x86-64 natives on the same thread and host tables to a guest after
  a nested host native; guest and host now get separate proxies;
- the struct-layout, syscall-number, auxv, setjmp and va_list differences
  were not handled (guest_libc.cc);
- guest objects could bind to x86-64 objects' symbols (and vice versa);
- the overhead figure (×1.3–1.6) compared against a native loop slowed by a
  `volatile` store per iteration; measured honestly it is far higher
  (docs/arm64.md).
