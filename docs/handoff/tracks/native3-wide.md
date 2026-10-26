# Track brief: native3-wide

Close the remaining wide/multibyte libc ABI contracts under native3/ADR 0062.
ADR 0073 is reserved for this subtrack. Branch wip/native3-wide, worktree
C:\work\gesher-native3-wide, from integration 94ccde1 (combined integration
ci 40416300047 is pending). Root owns all integration/main/release operations.

Read handoff/README.md, track-rules.md, native3.md, native-libs.md, posix.md,
ADRs 0011/0057/0062 and the exact android-16.0.0_r4 bionic implementations
before editing. Follow the existing code style and required commit footer.
All compilation and native/Java fixture execution goes to CI; local work is
source inspection, edits and lightweight script/parser checks only. No AOSP
build or WSL locally. Never touch the protected backup, use absolute paths
in this worktree, no stash, and watch only with the prescribed 120s interval.

## Scope

- Audit which duplicate wide/UTF-8 entries actually win registration in
  extra_bionic.cc and locale_bionic.cc; consolidate behavior rather than
  correcting a fallback callers never reach.
- Complete pinned-bionic UTF-8 encoding/decoding and restartable conversions over
  Android's 32-bit wchar_t and 8-byte mbstate_t. Cover split input, pending
  state, reset, NUL, length queries, bounded output and exact source-pointer
  updates. Match each pinned function's acceptance/rejection and Android errno
  for overlong forms, surrogates, bad continuation and out-of-range values,
  without consuming or writing beyond its contract. Preserve and explicitly
  test baseline quirks/asymmetries instead of imposing a stricter inferred
  Unicode policy or claiming general standards conformance from this work.
- Implement real mbrtoc16/mbrtoc32 and matching C11 conversion/state behavior
  where missing, including supplementary UTF-16 pairs, pending output and the
  pinned state/NUL behavior. Derive unusual boundaries from a reference CI
  before choosing behavior; do not silently repair an upstream quirk.
- Finish wide integer conversions and intmax aliases, signed/unsigned 64-bit
  LP64 bounds, base detection, overflow/underflow and full-width end pointers.
  Audit double/float conversion and missing locale-suffixed forms against the
  pinned baseline. Do not narrow Android long to Windows' 32-bit long.
- Verify C-locale wide collation/transform and locale aliases, including
  supplementary values and bounded transform semantics. Match bionic's real
  locale support; Windows' changing language collation is not an oracle.
- Implement/verify wcsftime/wcsftime_l and relevant wide time/locale layout,
  including UTF-32 output units and bounded buffers. Coordinate any narrow
  strftime/tm-layout correction with root; locale persistence/Windows language
  events remain locale2's scope.

The baseline inventory remains grouped under the native3 umbrella; do not
rewrite historical counts or move other owners' entries. Add scoped verified
coverage to relevant entries only after CI. No stub exports to improve counts.

## Boundaries

Do not change Unicode classification/case mapping, wcwidth/wcswidth or their
locked ICU4X oracle: root verified these independently, and the current CPU-
clock continuation will integrate them. Do not edit ART patch 0572, its host
clock test, executable TLS, properties, logger, camera or shell. Binary128
long-double conversion/libm, wide scanf/stdio varargs and the remaining POSIX
contracts stay on native3; never claim a double-based long-double path is fixed.
Shared files receive minimal additive edits and are listed in the report.

## Verification

Reserve tests/windows/tracks/66-native3-wide.ps1. Extend the NDK/native2
fixture or an explicit focused artifact for both x86_64 and arm64. Exercise
the actual shim through dalvikvm and APKs in single-process/device modes,
checking return values, errno, guard buffers, state and pointer positions.
Use pinned upstream source/reference behavior to derive difficult boundaries;
do not compare only against a copy of the implementation under test.

Before final full CI merge current origin/phase4/integration. Personally read
every job, both zero-failure summaries and claimed PASS lines, installed-copy
input/uninstall cleanup and both required e2e logs. Tick only exact verified
contracts with the run ID. Keep this status current at meaningful pushes.

## Status and next steps

Prepared; scoped reference/compilation verified, production runtime pending.
The effective multibyte registrations now
consolidate in locale_bionic.cc; the unused extra_bionic conversions are
removed. State uses the actual four sequence bytes and preserves LP64's
four reserved bytes. C11 conversions preserve the pinned encoder/decoder
asymmetry and C16/NUL quirks, rather than imposing extra Unicode restrictions.
Integer parsing removes the 255-character limit and handles LP64 bounds,
binary prefixes and end pointers directly. Added locale aliases, bounded
C collation/transform and UTF-32 wcsftime over the existing narrow formatter.
No narrow time or tm-layout implementation was changed.

Independent CI reference sources are saved verbatim, with licenses and
SHA-256 manifest, under probes/native3/wide/upstream. Include-only adapters
compile the pinned conversion/legacy/integer bodies on Linux and run shared
contract assertions. native3-wide.yml also compiles the production locale
object with pinned llvm-mingw/-Werror and both NDK library ABIs. Compilation
and fixture execution are CI-only. test66 asserts each actual ABI/mode log;
production runtime and full CI remain pending.

Scoped run 40419128672 at 9bff75d failed before runtime: the production
object needed the C++ cstdlib header for std::malloc/free, and GCC rejects
bionic's Clang-oriented always_inline declaration under -Werror. Both are
corrected (the reference uses Clang, as bionic does). The NDK compile job
succeeded; no runtime or full-green verification is claimed.

Scoped 40419236055 at f280450 compiled production and both NDK ABIs, but
the reference compilation found Clang's suspicious-literal-concatenation
warning in a malformed-input fixture. The byte string is now explicit;
no assertion or production behavior was relaxed. test66 now checks the
actual test44 process logs for all six ABI/mode cases, avoiding duplicate
Activity runs. No runtime proof is claimed until the next scoped/full run.

Scoped 40419402781 at 908125d is green; all three actual job logs were read.
The pinned reference prints "PASS pinned wide: 186 conversion/state/boundary/
LP64 assertions, 0 failures". Production locale compiles with pinned
llvm-mingw/-Werror, and both NDK library ABIs compile. This is independent
expected-behavior/compile evidence, not production runtime proof. The next
scoped run adds the actual pinned wcstod/parsefloat grammar reference with
an explicitly limited host rounding/finite-stream adapter and locale-alias
calls. ADR 0073 records the registration audit, quirks and open boundaries.

Expanded scoped ci 40420020504 at 9ea7ef5 is green; all three job logs read.
The reference now prints "PASS pinned wide: 202 conversion/state/boundary/
LP64 assertions, 0 failures", including the original wide floating grammar.
Production locale and both NDK ABI builds also pass. Current integration
a06d930 is merged as 6bec63d before the first full runtime CI dispatch.
No production runtime, package or e2e claim is made yet.

First full ci 40420123819 at 6e91b8d is pending. A final float audit adds
a decimal just above a binary32 halfway boundary (detects double rounding)
and explicitly selects the CRT's C numeric locale for wcstod/wcstof, matching
both supported bionic locales independently of host language changes. This
follow-up needs scoped compilation/reference and a subsequent full run;
the first full run remains useful production diagnostics for the prior SHA.

Scoped ci 40421142590 at a898a11 is green, all three job logs read:
"PASS pinned wide: 203 conversion/state/boundary/LP64 assertions, 0 failures".
Both NDK ABIs and the production locale object compile with the new parser
calls. Superseded full 40420123819 was cancelled during compilation, before
Windows runtime results; no failure or runtime proof is inferred from it.
origin/phase4/integration remains a06d930 and is included. The next full run
uses this verified follow-up and is the required production acceptance run.

Root separately verified Unicode width in ci 40404678100 at 581e88f; this
track does not modify its classification/case/width implementations or
ART patch 0572. Final verification must first include current integration.
