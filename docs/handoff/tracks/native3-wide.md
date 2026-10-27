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

Current acceptance run: **full ci 40425393716 at 57782e5**, pending.
Implementation is frozen at eb9f964 (the later commit changes docs only),
with all 248 independent assertions and compilation verified in scoped
40425021540. Fifteen completed build-job logs have been personally reviewed;
the main runtime build is still running, so no Windows test/package/e2e
acceptance result is claimed. Continue watching at interval 120, then inspect
all remaining logs and test66's six cases per runner before updating coverage.
Integration a06d930 is included; root's unsigned-width continuation remains
separate and must retain both guest overrides when it is integrated.

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

Full ci 40421328934 at 27f9391 is pending. A separate source audit is
checking the NDK's char signedness because pinned localeconv stores
CHAR_MAX sentinel bytes; the shared Windows layout currently uses 127.
The scoped workflow now prints both target macro sets before its ordinary
fixture builds. Root was notified before any architecture-specific edit.

Scoped ci 40421702672 at 611f520 confirms the ABI difference: arm64 defines
__CHAR_UNSIGNED__/__WCHAR_UNSIGNED__, while x86-64 does not; long is 64 bits
for both. Prepared a localeconv-only guest_libc override (the sole cross-domain
production edit) returning 255 sentinel bytes while sharing all real pointer
fields. NDK tests now check all fourteen fields, exact lconv layout and
import/dlsym identity in both ABIs. Scoped compilation now includes this
guest object with -Werror; it and the subsequent full runtime run are pending.

Scoped 40421984640 at e0077b3 passed the locale and guest objects with
-Werror, both NDK fixtures and the 203-case reference; actual logs read.
Root accepted the localeconv-only cross-domain override. A further signedness
audit found that pinned LLVM-libc comparisons return raw 32-bit subtraction;
the common wcscmp/wcsncmp/wmemcmp and collation now preserve that magnitude
and high-bit result. Exact signed/unsigned LLVM comparison bodies and the
unsigned bionic numeric-template instantiation extend the independent CI
reference. Production numeric tests cover high-bit source/end behavior.
No width/property/case code or fixture was edited; root owns its unsigned
width-reference correction. This follow-up still needs scoped/full CI.

Reference provenance checking now compares every saved source with the
actual pinned upstream bytes in CI, in addition to the checked-in digest.
This also prevents a failed source fetch from leaving a misleading file:
an erroneous LLVM LICENSE.TXT path (upstream uses LICENSE) was removed;
its copied content was never compiled. The genuine LLVM LICENSE is retained.

Scoped ci 40423004909 at 569b497 is green; all three actual job logs read.
Every saved source/license matches its pinned upstream bytes. The reference
prints "PASS pinned wide: 237 conversion/state/boundary/LP64 assertions,
0 failures", including signed/unsigned LLVM comparisons, high-bit numeric
input, hard float rounding and the earlier state/bounds contracts. Production
locale and the localeconv guest adapter pass -Werror; both NDK ABIs compile.
Superseded full 40421328934 (pre-lconv/comparison source) was cancelled in
compilation; it supplies no runtime acceptance claim. Current integration
remains a06d930 and is included before the next complete acceptance run.
Root's unsigned width follow-up is separate and has not been modified here.

Full ci 40423326965 at 76cf72e is pending. A final source comparison found
one wrapper error-order mismatch: invalid wcsftime format must report EILSEQ
before a SIZE_MAX destination reports EINVAL. The wrapper now follows the
pinned allocation/conversion order. Shared tests and the original FreeBSD
wrapper reference cover invalid/empty formats, zero/overflow bounds, Unicode
zone output and malformed zone bytes. The reference uses a host narrow C
formatter only; narrow strftime/tm code remains unchanged. This correction
needs scoped CI and the subsequent complete runtime acceptance run.

Scoped 40424755605 at ff72ac0 stopped before reference execution because
C permits an uninitialized static const aggregate declaration that C++
requires to be const-default-constructible. The reference mbstate adapter
now has an explicit zero constructor, preserving C's initialization while
leaving the original time body unchanged. No assertion was relaxed; the
new time/error-order source is not yet runtime verified.

Scoped ci 40425021540 at eb9f964 is green; all three actual logs read.
The reference prints "PASS pinned wide: 248 conversion/state/boundary/LP64
assertions, 0 failures", including the original wide-time wrapper's own
buffer/error logic and UTF-8 zone conversion. Provenance verification,
production locale/guest -Werror and both NDK compilations pass. No further
implementation work remains from the source audit; production acceptance
still requires the next full run. Superseded 40423326965 was cancelled while
the main runtime was compiling and supplies no Windows runtime result.
Current integration a06d930 was fetched/rechecked and is already included.

Root separately verified Unicode width in ci 40404678100 at 581e88f; this
track does not modify its classification/case/width implementations or
ART patch 0572. Final verification must first include current integration.
