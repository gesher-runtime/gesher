# Pinned bionic Unicode reference

This CI-only reference uses Android's ICU4X 2.0.0-beta2 property and simple
case-mapping crates. Every ICU dependency, including its data, is pinned to
that prerelease. Cargo.lock must also be recorded before this reference is
used to verify production behavior.

src/bionic_icu4x.rs is unmodified from
https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/icu4x.rs
(SHA-256 bf91cf92abec64697acaaf845fbca56c883ff2b9bdae8281116944791f901e53).
It retains its upstream copyright and SPDX license.

main.rs applies pinned libc/bionic/wctype.cpp's ASCII branch and category
bounds around that wrapper. It emits a separate FNV-1a-64 digest of each
normalized predicate or simple case mapping, over every value 0..0x10ffff
and then four out-of-range values. Each output value contributes four
little-endian bytes. Boundary rows include ASCII vertical tab versus U+001C,
combining U+0345, Roman numeral U+2160, supplementary letters, Unicode 16
additions, surrogates and WEOF. This reference does not use Windows' ICU.

The current workflow bootstraps the dependency lock and prints its exact
bytes in a base64 log record, allowing the coordinator to save the lock
without downloading an Actions artifact. After that bootstrap, dependency
resolution must use --locked. A successful reference run alone does not
implement or verify Gesher's Unicode functions; the production bridge and
both Windows/NDK ABI checks are still required.
