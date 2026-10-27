# 0079. ARM variadic-list conversion

Status: investigation; no production correction or runtime acceptance.
Windows test86 is reserved. Every compilation and executable fixture runs
in CI. Source starts from verified integration3577b330 with the coordinator's
docs-only checkpoint13603d54.

## Evidence and boundary

Surface full40507882249/06d30d40 fails both ARM test80 modes before the ABI
marker. Exact pinned-symbol run40523288209 identifies pc0x2544 as the return
from vprintf@plt at0x2540. That identifies a call seam, not a cause.
Read-only source review finds eager stack copying in ConvertVaList and mixed
GP/FP overflow ordering limits already recorded in ADR0047. Actual guest
stack/list bounds have not yet been observed.

The predecessor's opt-in GESHER_ARM64_VARARGS_TRACE diagnostic and original
Surface printf/vprintf logger with separate shallow/deep controls are saved
unchanged in this WIP checkpoint before further implementation. Their scoped
CI workflow pins the independently accepted runtime40498782924/8ec4bf3c and
overlays a current CI-built translator and guest probe. A diagnostic baseline
success would establish only its explicit observed defect/control assertions.
It cannot establish product acceptance or substitute for the original Surface
tests. The deep frame is a separate control and must never become a fix.

## Next step and proposed scope

First read actual baseline CI logs. Then evaluate a tagged private register
save representation with demand-driven shared shim NextGp/NextFp reads,
separate GP/FP banks, one ordered overflow area and independent copied-list
state. A DLL/shim representation change requires an API-version compatibility
guard and explicit mismatch tests. Ordinary x86 SysV behavior must remain
unchanged. Keep original test assertions and the Surface logger.

Require real zero/few/many integer/FP/stack calls, mixed ordering, shallow/deep,
nested callbacks, independent va_copy and threads on both Windows runners.
Retain binary128, direct-variadic and HFA limits until separately implemented
and verified. No roadmap checkbox is changed.
