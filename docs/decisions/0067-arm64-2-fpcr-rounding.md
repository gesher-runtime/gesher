# 0067. arm64-2: guest FPCR rounding across native boundaries

Status: proposed. First scoped ci 40442317928 at e7c2878 passes both
Windows host/translator tests. Extended NDK contracts and full CI pending.
This is the first rounding slice of arm64-2, not completion of that track.

## Context

The guest fenv wrappers accepted only rounding mode zero and returned zero
for every query. The pinned bionic arm64 implementation supports modes
0..3 and updates only FPCR bits 23:22. Direct guest MSR instructions can
also change FPCR. Dynarmic already exposes its real GetFpcr/SetFpcr state,
but each guest nesting depth owns a separate Jit.

## Decision

Use the real current Jit FPCR for fesetround/fegetround. Preserve all other
supported FPCR bits and reject invalid modes without changing state.
When a guest calls host code or makes a raw syscall, halt at its saved next
PC, execute the host operation after Run restores native MXCSR, and resume
through a fresh dispatch. This avoids returning into a block specialized
for an old FPCR value or executing arbitrary host code in guest rounding.

Nested guest calls inherit the parent Jit's actual FPCR, including direct
MSR updates, and propagate their final FPCR back on return. The outermost
guest state persists per thread independently of the host fenv. A narrow
guest pthread_create wrapper captures FPCR for the new thread; common
pthread code and guest TLS layout remain unchanged. Native fenv guards
restore the host environment after guest entries and native callbacks.

## Verification and limits

The CI-built NDK assembly test performs actual MSR/MRS and positive/negative
FADD instructions. Its native Windows caller checks all four modes,
non-rounding bits, invalid-mode errors, concurrent isolation and a three
guest-level callback chain that changes both guest and native rounding.
Both Windows versions run this and the original translator smoke tests in
the branch-only arm64-fpcr workflow. Full CI uses reserved test69.

Actual logs of ci 40442317928 were read: production translator/guest objects
compile with -Werror; both Windows versions print all four FPCR groups and
0 test(s) failed, and the original translator smoke tests pass. This covers
real positive/negative FADD, invalid modes, direct MSR and three guest nesting
levels, native fenv restoration and concurrent guest state. It is not full
NDK/APK or product acceptance.

The next source adds the full licensed bionic fenv-arm64.c and ARM header,
with actual-tag byte provenance checks. The unchanged body is compiled as
guest code with renamed symbols, so its real MSR/MRS can be compared against
shim calls. Shared NDK contracts add FADD/FRINTI, errno, pthread inheritance,
concurrent modes and nested qsort callbacks to native2's ARM library and an
actual ELF process. test69 consumes actual dalvikvm/single/device logs.
These extensions await scoped/full CI. No local compiler or native/JVM
fixture ran.
FPSR exception/status/trap APIs and environment save/restore functions
remain outside this slice; their old fallback implementations are not
claimed as complete. Guest JIT pages, signals/ucontext, LSE, HFAs/variadic
binary128 and cache/stack limits remain open arm64-2 contracts.
