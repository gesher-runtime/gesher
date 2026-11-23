# Native unwind records for the ARM64 ABI thunks

## Contract and scope

The two handwritten x64 thunks used by the ARM64 translator allocate native
stack frames and call other native functions. Their original COFF object had
no unwind records. Windows therefore could not reliably reconstruct their
callers during a stack walk or native exception unwind.

Microsoft's [x64 exception-handling specification](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64)
requires records for nonleaf functions and defines save locations relative to
the fixed frame base. Its [prolog and epilog rules](https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog)
also require instruction offsets to describe partial prologs, and constrain
the instructions recognized as epilogs. The native test uses the documented
[RtlVirtualUnwind API](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlvirtualunwind)
on captured contexts.

This is a standalone prerequisite for #56 N15/N18. It changes native frame
description, without changing Android arguments, return channels, permissions,
capabilities or callback control flow. It does not implement longjmp across
nested native/guest callbacks, establish C++ resource cleanup, or prove an
exception can safely escape an arbitrary native API.

## Frame representation

Let S be the stack pointer on entry, pointing at the return address.

| Thunk | Saved GPRs | Fixed stack base | Final RBP | Frame offset |
| --- | --- | --- | --- | --- |
| gesher_call_host_sysv | RBP, RBX, RSI, RDI | S - 200 | S - 8 | 192 |
| gesher_guest_entry_shim | RBP | S - 168 | S - 8 | 160 |

The host-call thunk records the four pushes, its fixed 168-byte allocation,
and all 128 bits of XMM6 through XMM15 at offsets 0 through 144. RBP is now
established after that allocation, at the same final address as before.
Variable System V stack arguments remain below the fixed frame and unwind
through RBP. The guest-entry thunk similarly records its RBP push and
160-byte EntryFrame; its subsequent 32-byte Microsoft shadow allocation is
a body adjustment recovered through the frame pointer.

Argument mapping, result slots, ordinary restoration instructions and final
stack layout stay unchanged. The existing XMM restores precede the host
epilog's add/pop/ret sequence. The guest result loads likewise precede its
fixed-frame add/pop/ret sequence.

## Standalone native validation

The test links the exact production thunks.S with small legal native callers
and targets. It needs neither Dynarmic nor an Android runtime.

- Lookup must find each production function's exact range. The unchanged
  assembly from main e80cd185 is compiled into a separate negative executable;
  it must fail this lookup cleanly, before any broken frame walk.
- A native called target captures its actual CONTEXT and walks its real helper
  frames to the production thunk. Unwinding a copy must recover the known
  caller PC/RSP and saved registers. The host target deliberately clobbers
  System V volatile RSI/RDI and XMM6–XMM15, while each saved register and each
  64-bit XMM lane starts with a distinct value.
- The host cases use 0, 1, 2, 16 and 31 stack words, verifying alignment,
  variadic AL, six integer arguments, eight FP arguments, all stack arguments
  and RAX/RDX/XMM0 results. The guest case verifies integer arguments 1–9,
  eight FP arguments, shadow space and the same result channels. Normal
  returns separately check preserved state and stack balance.
- The build derives instruction boundaries from the production object.
  Temporary single-byte breakpoints capture real interrupted prologs,
  dynamic stack adjustments, call sites, result restoration and every epilog
  instruction. The original instruction is restored before Windows examines
  the code, and only a copy of the exception CONTEXT is unwound. Execution
  then resumes the original instruction.
- A second source-derived negative changes only XMM15's unwind save offset
  from 144 to 128. The native assertions must detect the wrong full-width
  restored value; an unrelated crash or timeout is not accepted.

The added arm64-native-unwind workflow cross-compiles with the existing
llvm-mingw 20261006 release, verifies the archive SHA256, and runs on both
windows-2022 and windows-2025. Each executable has a 60-second limit.
Build receipts contain exact source commit/tree/file hashes, compiler identity,
commands, object/executable hashes, disassembly and decoded unwind records.
Native receipts bind the current run/attempt, downloaded artifact, actual OS,
binary hashes, exit codes and output hashes. The controls require their
specific failure diagnostics and exit codes.

This focused gate supplements the existing full CI. Cross-compilation and
decoded metadata establish build properties only; successful execution on
both actual Windows runners remains required before merge. These tests use
the real Windows unwinder and production assembly, but do not execute the
translator, ART, guest callbacks or cleanup-aware nonlocal transfer.
