# 0067. arm64-2: guest FPCR rounding across native boundaries

Status: proposed. Combined scoped ci 40443927632 at 053bbc0 passes exact
source provenance, NDK builds and both Windows host/translator tests.
Full JNI/APK/native-process and product acceptance remain pending.
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
Extended scoped ci 40443134770 and combined scoped 40443927632 are green;
all five successful logs of each were read. The original bodies and actual
JNI/APK/native-process artifacts compile, while both Windows host fixtures
pass every FPCR group and zero-failure summaries. The combined candidate
also retains current integration 8922a3e's verified clocks/widths and the
frozen verified wide/C11 prerequisite e7c1b06. Actual NDK process execution
and the full suites/package/e2e still require full CI. No local compiler or
native/JVM fixture ran.
FPSR exception/status/trap APIs and environment save/restore functions
remain outside this slice; their old fallback implementations are not
claimed as complete. Guest JIT pages, signals/ucontext, LSE, HFAs/variadic
binary128 and cache/stack limits remain open arm64-2 contracts.
Rounding-sensitive host CRT/libm calls are a separate open contract; this
slice verifies translated guest arithmetic and the FPCR rounding entries.
