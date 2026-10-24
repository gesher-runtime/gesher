# 0036 — Stack overflow, compiled JNI stubs and placeholder mappings

**Status:** accepted, 2026-10-24 (stack track). Extends ADR 0033 and 0005.

## Context
ADR 0033 brought nterp, the JIT and AOT code to Windows and left four gaps
(docs/compiled-code.md, "Not done yet"):

1. **Deep recursion** under nterp, the JIT or AOT code ended the process with
   `STATUS_STACK_OVERFLOW` instead of throwing `StackOverflowError`. ADR 0033
   put that down to ART's fault handler running on the exhausted stack (there
   is no `sigaltstack`). Measuring it (a probe that recurses with ART's
   `testq %rax, -32K(%rsp)` stack probe, `probes` in the track's notes)
   showed another cause: Windows grows a stack with a guard page that moves
   down one page per fault, and *every* fault on it makes the next page down a
   guard page, whatever that page was. ART's protected region at the bottom of
   the stack (committed, no access) is turned into a guard page and then into
   an ordinary page as the guard passes it; when the guard reaches the stack
   guarantee (the lowest page plus two on x64), the kernel commits what is
   left and raises `EXCEPTION_STACK_OVERFLOW` on the instruction that touched
   it - a stack probe 32 KiB below the stack pointer, with all the stack the
   handler needs above it. Nothing delivered that exception to ART.
2. **JNI stubs.** The JNI compiler's macro assembler reached `Thread*` through
   `%gs`; native methods used the generic JNI trampoline.
3. **Mappings.** gesher-base's `mmap` could not put a file view where it wanted:
   Windows maps views only into free address space (or 64 KiB-aligned), so an
   oat file's segments, mapped `MAP_FIXED` into ART's reservation, were
   *copied*, and executable file views were refused. The JIT's dual-mapped
   (W^X) code cache needs two views of one file, one of them executable, and
   one of them replacing part of another (`RemapAtEnd`).
4. **AOT for the boot image extensions** (framework.jar, gesher-app.jar):
   compiled code there is 75 MB for framework.jar, which copying into every
   process would make startup slower, not faster.

## Decision
- **`EXCEPTION_STACK_OVERFLOW` is a `SIGSEGV`** at the address the exception
  carries (`fault_signals.cc`), when that address is a probe (more than a page
  below the stack pointer; native code that overflows is left to the crash
  report as before). ART's `StackOverflowHandler` recognizes its probe
  (`sp - gap`) and throws `StackOverflowError`; it unprotects and protects its
  region again around the throw, which puts the region back. From then on the
  whole stack is committed and the next overflow on that thread is an access
  violation on the region. No alternate stack: the handler runs below the
  probing frame, 32 KiB above the region.
- **ART's protected region sits on the guarantee.** `pthread_attr_getguardsize`
  reports the lowest page plus the thread's stack guarantee (two pages by
  default, `SetThreadStackGuarantee` otherwise), which ART adds to the stack
  base; the region is then the page whose touch raises the exception, and
  stack growth (`GrowStack`) stops above it.
- **JNI stubs reach `Thread*` through the TLS slot** (ART patch 0016): each
  Thread field access in the x86-64 JNI macro assembler loads `Thread*` from
  `%gs:0x1678` into R10 first - R10 is no argument, return value or
  callee-save register of either calling convention and the stubs use it for
  nothing else. JNI compilation is back on for dex2oat and the JIT; the one
  C++ entrypoint the stubs call, `pJniDecodeReferenceResult`, gets a System V
  wrapper.
- **Placeholder mappings** (gesher-base `mman.cc`, Windows 10 1803 and later):
  every mapping is a placeholder reservation filled with an allocation
  (`VirtualAlloc2`) or a view (`MapViewOfFile3`), both at page granularity.
  `MAP_FIXED` over pages of a mapping turns them back into a placeholder (an
  allocation gives pages up with `MEM_PRESERVE_PLACEHOLDER`; a view is
  unmapped and the pieces that stay are mapped again, with their protections
  and their written private pages) and fills it. Executable views reopen the
  file with `GENERIC_EXECUTE` (`ReOpenFile`). A view covers whole pages of the
  file (Windows refuses a view past its end); the rest of a private mapping
  is anonymous memory with the file's bytes. Without placeholders the old
  allocation/view/copy behaviour stays (`GESHER_MMAP_NO_PLACEHOLDERS=1` forces
  it).
- **The JIT's code cache is dual-mapped** (ART patch 0017): `art::memfd_create`
  is gesher-base's (a delete-on-close temporary file), so ART takes its W^X
  path: code is written through a read-write view and runs from a separate
  read-execute view of the same file; no page is ever writable and executable.

## Consequences
- `StackOverflowError` is caught and recovered from under every execution
  mode, on the main thread and on new threads (tests/windows/tracks/70-codegen.ps1).
- Native methods called from compiled code go through compiled stubs (the
  boot image's AOT stubs, the JIT's for app natives).
- An oat file's code is a view of the file, shared between processes until
  written, instead of a private copy in each process. That makes AOT for the
  extensions affordable: boot-image.yml compiles framework.jar and
  gesher-app.jar with `speed` too (298,365 methods with compiled code in all,
  75.7 MB of oat file for framework.jar), with no measurable startup cost.
- Without placeholders, gesher-base's `memfd_create` fails for ART (ENOSYS)
  and the JIT keeps its single mapping.
- Each thread gives ART 8 KiB less stack (the guarantee is now outside ART's
  usable stack); `GrowStack` grows down to ART's region instead of stopping
  64 KiB above the bottom.
