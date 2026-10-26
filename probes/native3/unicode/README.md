# Pinned bionic Unicode reference

This CI-only reference uses Android's ICU4X 2.0.0-beta2 property and simple
case-mapping crates. Every ICU dependency, including its data, is pinned to
that prerelease. Cargo.lock was recovered from the successful reference job's
log in ci 40374405950 at c5a9f95, with SHA-256
327e35387c66cc9c3e44f873ecc124cf21888ad602ce668e697fd0415618a392.
All eight ICU crates/data dependencies are exactly 2.0.0-beta2. reference.json
records that run's 14 digests and 11 boundary rows; it is reference output,
not evidence that the current Windows backend matches it.

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

The workflow uses --locked and prints the dependency lock's exact bytes in
a base64 log record, allowing the coordinator to audit it without downloading
an Actions artifact. A successful reference run alone does not
implement or verify Gesher's Unicode functions; the production bridge and
both Windows/NDK ABI checks are still required.

Character-width continuation: src/width.rs derives the exact decision order
from android-16.0.0_r4 libc/bionic/wcwidth.cpp (SHA-256
401b0bbd9349d3209a990ab25cb23741248a343c543bc99fc7e4bb224a6b74fa),
retaining its license. It uses the same unmodified wrapper and lock above,
including signed wchar_t inputs, the U+00A0 boundary, Hangul syllable type,
default ignorability, format/mark categories and East Asian Width. The
--width mode emits a whole-code-point digest and 33 boundary rows for initial
CI review; those values are not yet frozen or a production verification.
