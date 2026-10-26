# 0073. native3-wide: pinned multibyte and LP64 wide ABI

Status: proposed; independent conversion/integer/floating-grammar reference
and compilation verified in ci 40421142590 at a898a11. Production runtime
awaits full CI. This does not close native3/0062.

## Context

Android's wchar_t and long are 32 and 64 bits; the Windows CRT uses 16 and
32 bits. The existing shim split multibyte behavior between two files:
RegisterExtra runs first, then RegisterLocale replaces only some names.
Correcting extra_bionic's decoder alone would leave mbrtowc's actual entry
unchanged. Wide numeric parsing also narrowed through a 256-byte buffer,
losing long-input values and source positions.

## Decision

Consolidate multibyte/C11 conversions, legacy wrappers and wide collation
in RegisterLocale, removing the duplicate extra_bionic implementations.
Preserve the exact android-16.0.0_r4 contracts, including its asymmetric
or surprising cases, rather than applying a different UTF policy:

- mbrtowc/mbrtoc32 reject overlong forms, surrogates, invalid continuation
  bytes and decoded values above U+10FFFF. Partial calls retain the first
  three sequence bytes; completion/error resets those four bytes only.
  LP64's four reserved mbstate_t bytes remain untouched; mbsinit ignores them.
- c32rtomb/wcrtomb accept encoded surrogate values and values up to
  0x1FFFFF, although their decoder rejects those byte sequences. Pending
  state is rejected except for NUL/null-output reset behavior.
- mbrtoc16 emits the high surrogate with the current input-byte count and
  returns -3 for the queued low surrogate without consuming more input.
  Pinned NUL conversion leaves its output pointer unchanged.
- c16rtomb preserves the pinned high-surrogate byte representation. U+D800
  has a zero payload and leaves initial state; a following low surrogate
  therefore fails with EINVAL. This is a compatibility quirk, not a claim
  that every UTF-16 pair is accepted by the Android baseline.
- mbsnrtowcs treats a bounded incomplete character as EILSEQ, advances a
  real source pointer by its available byte count and resets state. A
  measurement call leaves the source pointer unchanged. Legacy mbtowc
  retains incomplete state after reporting EILSEQ; mblen exposes -2.
- wcsrtombs advances after every completed wide character, leaves a NUL
  pending when the byte buffer is exactly full, and never emits a partial
  multibyte sequence. Measurement ignores the output bound.

Parse wide integers directly with fixed 64-bit arithmetic. Preserve bionic's
base-0/hex/binary rules, unsigned negation, saturation/ERANGE and full-width
end pointers; long, long long and intmax variants share their LP64 entries.
Floating parsing uses the pinned wide whitespace/candidate alphabet and an
unbounded ASCII buffer and explicit host C numeric locale before direct
binary32/binary64 conversion. A decimal just above a binary32 halfway value
checks that the float path does not round through double. Binary128
precision is a separate open contract; the existing narrowed wcstold path
is not claimed as fixed.

Locale names/categories/masks and owned locale copies follow bionic's two
supported locales. Locale-suffixed wide conversions share the ordinary
semantics. C collation compares code points; wcsxfrm follows bionic's
wcslcpy alias, including termination of truncated output. Windows language
collation is not consulted.

wcsftime converts its UTF-32 format through the shared conversions, calls
the existing System V narrow strftime entry with Android's unchanged
56-byte tm, and decodes UTF-32 output. Preserve the pinned FreeBSD wrapper's
narrow byte bound (maxsize, despite a larger temporary allocation), size
overflow error and locale alias. This track changes no narrow strftime or
tm-layout code.

## Evidence and limits

probes/native3/wide saves the full licensed pinned sources and SHA-256
manifest. CI compiles their conversion/legacy/integer bodies verbatim with
include-only environment adapters and runs the same contract assertions as
the NDK libraries. The optional floating reference retains the original
wcstod/parsefloat bodies with a finite FILE adapter; host strtod/strtof
provides rounding, so it verifies pinned wide grammar/end handling rather
than claiming a complete independent Android gdtoa implementation.

Scoped ci 40421142590 verifies 203 reference assertions with zero failures,
the production object under pinned llvm-mingw/-Werror and both NDK library
builds. test66 consumes actual test44 process logs for x86_64/arm64 dalvikvm
and APK single/device modes. Their production runtime proof and final full
CI are still pending. No local native/Java compilation or fixture ran.

Unicode properties/case/width, binary128/libm, wide stdio/scanf and generic
POSIX remain outside this decision. The baseline inventory stays under
native3; symbol presence is not a full semantic-coverage claim.
