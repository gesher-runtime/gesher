# 0006 — Calling convention and thread register for ART's assembly

**Status:** accepted, 2026-10-20

## Context
ART's x86-64 runtime contains about 6,000 lines of hand-written assembly:
the quick entrypoints, the JNI and native trampolines, and nterp (the fast
interpreter). Code that ART compiles (JIT, AOT) follows the same conventions.
Two of them clash with Windows:

1. **Calling convention.** The stubs call C++ (`artQuickToInterpreterBridge`,
   `artAllocObjectFromCode...`, about 150 functions) and are called from C++
   (`art_quick_invoke_stub`) using the System V AMD64 ABI. Windows x64 C++
   uses the Microsoft ABI: different argument registers, 32 bytes of shadow
   space, `rsi`/`rdi`/`xmm6-15` callee-saved, different struct returns.
2. **Thread register.** On Linux ART points the `gs` segment base at the
   current `art::Thread` and reads fields as `%gs:OFFSET` (about 110 places
   in the assembly, plus every compiled method). On Windows `gs` is the TEB,
   and user mode cannot move it.

## Decision
- **Managed code keeps the System V ABI.** The assembly's calls stay as they
  are. On the C++ side, every function called from assembly or compiled code,
  and every declaration of an assembly symbol called from C++, gets
  `__attribute__((sysv_abi))` (clang supports it on Windows targets), through
  GESHER-gated macros and `#pragma clang attribute` regions in patches. The
  compiler then emits the conversion at each boundary, including floating
  point arguments and two-word returns.
- **JNI is System V too.** `JNICALL` is `__attribute__((sysv_abi))` in Gesher
  builds, and so are the function pointers of the `JNIEnv` and `JavaVM`
  tables (AOSP's `jni.h` declares them without `JNICALL`, so this is a
  libnativehelper patch). The generic JNI trampoline then calls native methods
  exactly as on Linux. That matches the x86-64 `.so` libraries of phase 6, and Gesher's own
  native libraries (libcore, framework) are built with the same `JNICALL`.
- **Thread register: a TLS slot read through `gs`.** At startup Gesher
  allocates one TLS index (`TlsAlloc`, required to be below 64) and stores its
  TEB offset, `0x1480 + 8 * index`, in the global `gesher_thread_self_slot`.
  ART's `Thread::InitTlsEntryPoints` path that would call
  `arch_prctl(ARCH_SET_GS)` stores `Thread*` in that slot instead. Assembly
  reads the thread with two loads:

  ```asm
  movq gesher_thread_self_slot(%rip), %reg
  movq %gs:(%reg), %reg          // reg = Thread*
  ```

  `%gs:THREAD_SELF_OFFSET` loads (the most common case) become exactly that.
  Other field accesses become a `Thread*` load into a free register followed
  by an ordinary memory operand; each site is ported by hand. The JIT/AOT
  code generators get the same rewrite later (phase 2+).
- **Object format.** The assembly is built as COFF by clang. DWARF CFI
  directives work as is (they produce `.eh_frame`). `.type`, `.size`,
  `.hidden` and `@PLT` get GESHER variants in `asm_support_x86.S`, as macOS
  already has.

## Consequences
- The managed side of ART stays identical to Linux, so its assembly and code
  generators need only the thread-register change.
- Every stub-to-C++ boundary must be annotated. A missed one corrupts
  arguments silently, so the list of C++ symbols referenced from assembly is
  checked by a build step against the annotated set.
- One `TlsAlloc` slot per process, and two loads instead of one to reach
  `Thread*`.
