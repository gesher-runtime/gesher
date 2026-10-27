# Pinned ARM FPCR reference

The full android-16.0.0_r4 bionic fenv-arm64.c and shared ARM fenv header
are saved verbatim with their original licenses and SHA-256 manifest.
CI verifies both saved bytes and the actual tag bytes. build-reference.sh
compiles the unchanged function bodies as real AArch64 code under renamed
symbols. Tests compare shim rounding calls with those bodies and direct
MSR/MRS, plus actual FADD/FRINTI results. There is no simulated register or
copied production parser in this reference. All native compilation and
execution are CI-only. Only FPCR rounding is claimed; the other reference
functions are renamed for linkage and do not imply FPSR/trap coverage.
