# ARM variadic-list conversion

Native agent-owned follow-up: C:\work\gesher-arm64-varargs,
wip/arm64-varargs from verified integration3577b330. ADR0079 and Windows
test86 are reserved. Full-environment/default-object work remains test85
under ADR0067; these reservations do not change either frozen Locale2 or
FPSR source. Every build and executable fixture belongs in CI.

## Status and next steps

Surface full40507882249/06d30d40 fails both ARM test80 modes before the ABI
marker. Symbol-only40523288209 independently disassembles the exact pinned
DSO: pc0x2544 is the return from vprintf@plt at0x2540; startPhases calls say
at0x23fc and returns0x2400. This identifies the logging-call seam, not the
cause of the memory fault.

Read-only source inspection found ConvertVaList eagerly copies62 stack
words when seven GP slots remain, including a literal-only formatting call;
Enter reserves256 bytes above its entry SP. Runtime SP/stack-top/list bounds
are not yet observed. ADR0047 also records incorrect mixed overflow ordering
when more than eight FP values precede GP-bank exhaustion. Do not hide these
contracts by padding, replacing the original logger or weakening assertions.

Coordinate exact diagnostics with Media. Evaluate a private tagged register
save layout and demand-driven shared NextGp/NextFp reads, preserving separate
GP/FP banks, shared stack ordering and independent copied list state. Any
DLL/shim representation change requires an API-version compatibility guard.
Keep ordinary x86 SysV behavior unchanged. Test real shallow/deep/nested/
thread calls with zero/few/many integer/FP/stack arguments, both positive
formats and the original Surface logging path. Retain explicit binary128,
direct-variadic and HFA limits until separately implemented and verified.
No full-CI acceptance, complete Surface causality or roadmap tick yet.

WIP checkpoint3b607e759de37a73f21e6aa1b59d200d48bb5f5d preserves the
predecessor's exact trace/probe/workflow and adds proposed ADR0079. Actual
diagnostic40525678026 is green (three success/one intentional reusable smoke
skip). Both actual baseline job logs were read:122265892154/Windows2022 and
122265892163/Windows2025. The unchanged literal Surface logger exits139 in
the shallow call, with seven GP/eight FP slots and eager_bytes496;
eager_end is exactly stack_top+0xa0 on each host, and the reported guest-read
fault is at top (2022) or top+0x80 (2025). The separate deep control exits0,
prints the full original literal and completes arm64_elf_test within the
actual guest stack bounds. This establishes that diagnostic valid-call
eager-copy defect; it does not establish the full Surface failure's cause.
Native runtime provenance is pinned40498782924/8ec4bf3c, while the actual
trace DLL/probe are CI-built from3b607e75. No local artifacts or execution.

The next source replaces eager copying with a private tagged host list:
immutable GP/FP bank end pointers, independently copied offsets and one
shared-overflow cursor per list. Shared NextGp/NextFpArea read requested
slots only. Audited direct consumers comprise shim_internal.h and the
stdio binary128 source reader, now routed through NextFpArea while retaining
its existing narrowing/alignment behavior. gzvprintf's copied-list retry,
wide forwarding and JNI Args use these same readers. API3 rejects older
hosts before initialization; the host's exact API check rejects older DLLs.

New CI-only acceptance sources cover zero/few/80 GP arguments, both overflow
orderings/interleaving, independent va_copy, original shallow/deep logger,
live-list qsort callbacks, two threads, wide output, binary128 register/stack
source regression and actual large gzvprintf retry. Scoped CI builds the
actual host loader/shim and translator; real API2/API3 mismatch executions
are required. Compile/runtime proof of this correction remains pending,
including JNI/APK/full-suite/package/e2e and original Surface acceptance.

Root's bounded source review found no further production blocker. Fixture
reviews corrected a wide/byte stream-orientation oracle, bounded every new
process/pipe wait and unconditional disposal, retained callback failures
independently of qsort's final order, and explicitly return scoped-driver0
after expected API mismatch exits. Original assertions remain. JNI primitive
and mixed-object V calls additionally compare independent va_copy invocations.

First correction scoped40527840216/d9bc86f3 is red before runtime: the real
new host/shim builds successfully in job122272149040 and the strict translator
compiles, but NDK guest compilation reports missing
tests/native2/arm64_varargs_main.cpp because the reusable sparse checkout
omitted tests/native2. Additive checkout correction928a09b7 is in fresh
scoped40528739976; all four actual executed logs have now been personally
read and both Windows jobs pass every required positive/negative control.
See [the exact scoped report](arm64-varargs-scoped.md). No previous red
runtime result is reclassified or waived.

The following source additionally includes the same contract body in Native2's
actual two-ABI JNI/APK library. Existing test44 requires its report in each
dalvikvm/single-process/device context without removing previous markers.
Reserved test86 runs the original shallow/deep body and both standalone ELF
consumers plus direct API initialization rejection. These additions require
their own compilation and actual coherent full execution. Locale2 full
40516054280 is red for three Windows2025 Web3 lifecycle/launch failures;
its source and the accepted FPSR leaf remain frozen. Full-environment/test85
work waits for stable Locale integration rather than changing either leaf.

Native2 compile-only40529197932 at64f33ff3 is now green, attempt1. Actual
job122277019774 and its metadata were personally read: both libraries/APKs,
javac/d8, original third-party/symbol pipeline and artifact publication pass.
No actual ART/APK execution or full product/Surface acceptance follows from
this compilation-only result. Production matches scoped928a09b7 exactly.
