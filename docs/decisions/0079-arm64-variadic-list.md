# 0079. ARM variadic-list conversion

Status: scoped correction verified at928a09b7 in40528739976, attempt1;
all four actual executed logs read, both real Windows consumer/compatibility
jobs pass. Actual JNI/APK/Surface and coherent full product acceptance remain
pending. See the [exact scoped report](../handoff/tracks/arm64-varargs-scoped.md).
Windows test86 is reserved. Every compilation and executable fixture runs
in CI. Source starts from verified integration3577b330 with the coordinator's
docs-only checkpoint13603d54.

## Evidence and boundary

Surface full40507882249/06d30d40 fails both ARM test80 modes before the ABI
marker. Exact pinned-symbol run40523288209 identifies pc0x2544 as the return
from vprintf@plt at0x2540. That identifies a call seam, not a cause.
Read-only source review finds eager stack copying in ConvertVaList and mixed
GP/FP overflow ordering limits already recorded in ADR0047. Actual guest
stack/list bounds are now observed in diagnostic40525678026/3b607e75.

The predecessor's opt-in GESHER_ARM64_VARARGS_TRACE diagnostic and original
Surface printf/vprintf logger with separate shallow/deep controls are saved
unchanged in this WIP checkpoint before further implementation. Their scoped
CI workflow pins the independently accepted runtime40498782924/8ec4bf3c and
overlays a current CI-built translator and guest probe. A diagnostic baseline
success establishes only its explicit observed defect/control assertions.
It cannot establish product acceptance or substitute for the original Surface
tests. The deep frame is a separate control and must never become a fix.

Actual baseline40525678026 succeeds on both hosts. Each original shallow
literal logger exits139 with eager_end=stack_top+0xa0 and a guest-read fault;
each separate deep control exits0 within the real guest stack bounds.
Exact jobs and evidence boundary are in the [track brief](../handoff/tracks/arm64-varargs.md).

## Proposed correction and remaining scope

Use a tagged private register-save descriptor with demand-driven shared shim
NextGp/NextFpArea reads,
separate GP/FP banks, one ordered overflow area and independent copied-list
state. A DLL/shim representation change requires an API-version compatibility
guard and explicit actual-host/DLL mismatch tests. Ordinary x86 SysV behavior remains
unchanged. Keep original test assertions and the Surface logger.

Require real zero/few/many integer/FP/stack calls, mixed ordering, shallow/deep,
nested callbacks, independent va_copy and threads on both Windows runners.
Retain binary128, direct-variadic and HFA limits until separately implemented
and verified. No roadmap checkbox is changed.

The layout preserves the host list's existing24-byte shape. Both bank end
pointers are immutable; independent copied-list offsets and overflow pointers
remain in each list. Conversion reads no bank payload or overflow slots.
Every direct bank-offset consumer was audited; NextFp128 now uses the same
FP source selector as double, preserving existing x86 binary128 stack
alignment and narrowing. No binary128 precision claim follows from this.
API3 is an intentional private representation break, so DLL initialization
rejects API1/2 before any shim call. The loader already requires exact API.

The bank/shared-stack semantics follow the normative list fields and
per-type argument selection in [AAPCS64 2025Q1](https://github.com/ARM-software/abi-aa/blob/2025Q1/aapcs64/aapcs64.rst#14appendix-variable-argument-lists).
The ordinary x86 representation and assembly variadic trampolines retain
their existing layout. Direct ARM variadic calls, HFAs and full binary128
remain explicit independent limits. JNI/APK and complete product/Surface
gates still require a coherent full run after scoped correction proof.
