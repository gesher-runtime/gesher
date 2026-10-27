# Independent scalar FP numeric/status oracle

Producer ci 40466512094 at 645febe8 is green; its actual job log was read.
The pinned NDK freestanding ELF executed in QEMU and all 8192 rows, exact
input tuples and arithmetic sanity controls passed. Matrix SHA-256:
`434b73d47dbd4d73fadda6a8879b0cf3fe4bf5a1419f5359148bc7b30b8210bb`.
QEMU binary SHA-256:
`4ccb5bea9e24502b0e938248e42e2aa68586a6b766d769de882b54c7350122da`.
This proves the independent producer, not agreement by the translator.

QEMU v9.2.4 at the exact commit in qemu.lock.json supplies independent ARM/
SoftFloat semantics. CI builds its unmodified AArch64 user emulator, records
the source pin, version and binary digest, and verifies cached binary bytes.
The same pinned NDK as the translator builds a freestanding ELF with actual
MSR/MRS and FADD/FSUB/FMUL/FDIV instructions; Linux write/exit syscalls only
serialize the result, with no libc or copied production fenv implementation.

Inputs cover 32/64-bit scalar values, all four rounding modes and DN/FZ
combinations, two cumulative FPSR seeds and 32 operand pairs: signed zeros,
normal boundaries, overflow/underflow, subnormals in both operand positions,
infinities and quiet/signaling NaN payload/sign/ordering. All 8192 raw numeric
results and status values come from execution. The verifier checks every
input tuple, complete output, simple exact arithmetic sanity and provenance.
It does not substitute hand-generated expected values for the oracle.

All compilation and native/emulated execution are CI-only. Local checks may
only parse/hash sources. This reference covers these scalar instructions;
it does not prove other instruction families, full fenv/default-object ABI,
exception traps, host CRT/libm or binary128 contracts.
