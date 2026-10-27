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
No production fix, runtime cause, full-CI acceptance or roadmap tick yet.
