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

Reserved, not started. Root has separately verified Unicode width in full
ci 40404678100 at 581e88f; that source is not yet integrated with the new
cloud/exec baseline. This subtrack starts from integration 94ccde1 and must
not reimplement the already verified width/classification work. The global
CPU-clock follow-up is owned by root and still needs combined CI.
