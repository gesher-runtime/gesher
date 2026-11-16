# 0084 — Raw system calls and code that is not on disk

**Status:** proposed, 2026-11-15 (native-libraries track)

Extends [0014](0014-native-libs-fs-base.md) (the load-time `%fs` rewrite).
Code: `compat/elf/fs_rewrite.cc`, `compat/bionic/syscall_bionic.cc`.

## Context
Apps protected by Chinese packers (Ijiami/爱加密, Jiagu, Legu, Bangcle) ship
a native "shell" library that runs first and decrypts the real app. On an x86
device (and Gesher reports `x86_64` first in `ro.product.cpu.abilist`), the
Ijiami shell of com.oceanwing.soundcore 3.9.7 picks `libexec_x86.so`, an
x86-64 library shipped inside `lib/arm64-v8a`: `s.h.e.l.l.S.a()` returns
`x86` when `Build.SUPPORTED_ABIS` contains it, and `N.x()` also reads the ELF
machine of `/system/bin/linker(64)`. Choosing it is right: it runs natively,
with no translation.

`libexec_x86.so` (and `libexecmain_x86.so`, and the arm64 `libexec.so`) are
packed with UPX ("$Id: UPX 6.00"). Its DT_INIT (`+0x8ec78`) is UPX's
decompressor; its code (FDEs from `+0x381d0` to `+0xe9ba0`, `JNI_OnLoad` at
`+0x6bf00`, libc++ exports at `+0xc3f60`…) exists only after DT_INIT has run:
on disk, `+0x381d0..+0x8ec78` is compressed data, and `+0x8f05a..+0xeea60` is
not in the file at all. The launch failed in two ways:

1. **The rewrite patched compressed data.** The loader rewrote every `%fs`
   access its decoder found in the FDE ranges: in compressed bytes. The 45
   "`%fs` access not rewritten" lines of the log were all there, each one an
   `int3` written into the data the decompressor was about to read.
2. **Raw system calls.** UPX's stub calls the kernel with `syscall`
   instructions (`mmap` 9, `mprotect` 10, `munmap` 11, `write` 1, `exit` 60).
   On Windows that instruction enters the NT kernel with the same number:
   `mmap` became NT service 9 (`NtRemoveIoCompletion` on current builds, with
   Linux arguments),
   which returned `STATUS_ACCESS_VIOLATION`, and `0xffffffffc0000005` passed
   UPX's `cmp $-0x1000,%rax` check as an address. `rep movsd` to it
   (`+0x8f028`) was the SIGSEGV with fault address `FFFFFFFFC0000005` in the
   crash report (rip = base + 0x8f028, rdi = 0xffffffffc0000005, r12 = the
   file's size). The stub also writes a `syscall; pop; pop; ret` stub at run
   time (for `munmap` of its scratch mapping) and enters it with `ret`.

`libexecmain_x86.so`'s `PT_GNU_EH_FRAME` (`0x18abdc`) is in no segment's file
bytes: reading it at load would fault in the loader itself.

## Decision
**Raw system calls are emulated.** The rewrite turns each `syscall` (0F 05)
it decodes into `ud2` (0F 0B, the same length) and records it. The loader's
vectored handler emulates a trap at a recorded site: `%rax` and `%rdi %rsi
%rdx %r10 %r8 %r9` go to the handler the bionic shim registers
(`SetRawSyscallHandler`), the result or -errno comes back in `%rax`, `%rcx`
and `%r11` are clobbered as the kernel does, and execution continues after
the instruction. The shim maps x86-64 numbers to its own functions
(`syscall_bionic.cc`); `rt_sigaction` converts the kernel's `struct
sigaction` to bionic's LP64 one; an unknown number is -ENOSYS. `syscall(2)`
uses the same table for numbers it had no case for (it returned ENOSYS).

**Self-unpacking objects are rewritten as they run.** An object is
self-unpacking when its `.eh_frame_hdr` is not in its file bytes, when at
least a quarter of its FDEs describe code outside its executable file bytes,
or when it carries UPX's `$Info:` string. Nothing in it is rewritten at load;
after the loader's final `mprotect`, its executable pages are made readable
but not executable (`DeferUnpackedCode`).

**Code made executable after the load waits for its first execution.** The
shim's `mmap`/`mprotect`/`munmap` go through the loader
(`MapLoadedMemory`/`ProtectLoadedMemory`/`UnmapLoadedMemory`): pages of a
loaded x86-64 object that loaded code makes executable (an unpacker's image,
a patch) are kept non-executable and recorded with the protection asked for.
The first execution in such a page is an execute fault; the handler decodes
from the faulting address, an actual entry point, along direct control flow
(staying in the object's pending and rewritten pages), and from the starts of
the object's FDEs in that page once its code was regenerated (for a packed
object, the FDEs describe the unpacked code, not the file bytes), rewrites
`%fs` accesses and raw system calls there, gives the page the protection
asked for and resumes. Stubs of such rewrites go into read-write-execute
memory near the object (other threads may be running earlier ones). A page
made writable or unmapped forgets what was rewritten in it.

Decoding from execution is what makes this safe for packed code: compressed
data is never decoded, because nothing executes it.

**Code a raw system call maps elsewhere waits too.** UPX 5's stub writes its
decompressor into a memfd and maps it `PROT_READ|PROT_EXEC` outside the
library, then runs it; it makes raw system calls of its own. Executable
memory that a raw `mmap`/`mprotect` (not libc's: a JIT's stays as asked)
creates outside every loaded object becomes an object of its own, without
unwind information, rewritten as it runs, and forgotten when unmapped.

**Copies of rewritten code.** UPX 6's stub (Ijiami's) copies itself into an
anonymous mapping and runs the copy, `ud2` and all. Each rewritten system
call keeps the bytes around it (within its page, as rewritten); a `ud2` at an
unknown address with the same bytes around it is a copy of that call,
emulated and remembered from then on.

**Shared file mappings over a loaded object.** UPX 5 `ftruncate`s a memfd to
its unpacked code's size and maps it `MAP_SHARED|MAP_FIXED` over the
library's text. gesher-base cannot place a shared view that ends in a partial
page; into a loaded object (or for a raw system call) such a mapping becomes
a private copy of the file's bytes, written back to the file by `msync` and
`munmap` (and when `MAP_FIXED` replaces it): UPX writes its unpacked code into
the mapping, msyncs and unmaps it, then maps the memfd again privately,
read-execute, to run it. gesher-base's `msync` now follows Linux: anonymous
or private memory succeeds, unmapped pages are ENOMEM, views are flushed.
Copies of a system call are recognized from at least 12 matching bytes
around the instruction, outwards from it: a copy can end right after it
(UPX copies its stub up to its last `ret`).

**`/proc/self/auxv` exists.** UPX 5 reads `AT_PAGESZ` from it with raw
`openat`/`read`, and its system call helper stops on `int3` when any call
fails (`cmp $-0x1000,%rax; jb; int3`): gesher-base now serves the auxiliary
vector getauxval reports.

**A memfd's final partial page keeps what a shared mapping wrote past
EOF.** UPX 5 ftruncates its memfd to the unpacked size (not a page
multiple), writes its return stub after the code, inside that last page,
unmaps, and maps the memfd again. Linux keeps those bytes in the page
cache; they never extend `st_size` and `pread` at EOF still returns 0. The
loader's copy of such a shared mapping now keeps a native view of the
file through its final page and writes whole pages back to it (msync,
munmap, mprotect, a replacing MAP_FIXED); `CopyFromFile` maps through that
page; `ftruncate` to the same size clears it, as Linux does. Before this,
the stub came back as zeros and the packed test library faulted at it.
UPX 5.3.0 builds the test library (earlier versions leave the x86-64
Android section header table past EOF, which Android's linker rejects).

## Consequences
- The UPX stub of `libexec_x86.so` unpacks itself; its `munmap` stub, reached
  only by `ret` but the first code to run in its page, is rewritten then.
- Code in a page that already ran, reached only indirectly (a function
  pointer, a `ret`) and not described by an FDE, is not rewritten. Its
  `%fs:0`/`%fs:0x28` `mov` loads are still recovered when they fault
  (0014); a raw `syscall` there would reach the NT kernel. The handler
  emulates raw system calls only at sites it rewrote.
- A trap (`ud2` and the execute fault) costs a kernel round trip; packers
  make few raw calls. Windows writes the exception frame below `%rsp`, as
  for the existing `int3` sites: a leaf function's red zone does not survive
  a trap in it.
- Code an app generates outside any loaded object (an anonymous RWX buffer)
  is not rewritten.
- `fork`, `clone`, `execve`, `rt_sigreturn`, `arch_prctl` and `getdents64`
  are not emulated (-ENOSYS).
- `ptrace` (a process nobody traces) and `regcomp`/`regexec` (over
  `std::regex`'s POSIX grammars) are now in the shim; Ijiami imports them.
  Regex compilation supports basic/extended syntax, `REG_ICASE`, `REG_NOSUB`,
  `REG_NEWLINE` (for patterns without literal newlines, searched per
  `\n`-delimited line), and `REG_PEND`, and normalizes BSD/Bionic redundant
  escapes of ordinary punctuation (such as `\/`) outside `[...]` brackets;
  `REG_STARTEND` honors bounded, non-terminated byte sequences. Unsupported
  compilation flags return `REG_INVARG` instead of silently using different
  matching rules.
- Copies of shared mappings publish their changes at msync, munmap,
  mprotect and replacement, not continuously to another copy of the file.
- Soundcore 3.9.7 now gets past its native libraries: both unpack, its
  application dex is loaded from memory, and ART then rejects a class whose
  method bodies the shell has not filled in (`VerifyError`: a constructor
  returning without calling its superclass's). The shell's dlopen/dlsym
  calls all succeed except probes for libraries a phone lacks too
  (`GESHER_TRACE_DL=1`, docs/compiled-code.md); what it expects of ART at
  that point is outside this decision; [0086](0086-packer-art-hooks.md) is
  that (a stand-in libart.so whose ClassLinker::LoadClass it hooks).
