# Bionic regex compatibility QA — 2026-11-17

> Historical review of the head identified below. The later repair replaces
> the custom compiler with a bounded adaptation of the pinned Bionic engine;
> see [2026-11-18 repair evidence](bionic-regex-repair-2026-11-18.md). Historical
> failures and conclusions below have not been rewritten as passing results.

## Decision and reviewed state

PR #60 is **not ready to merge**. The goal is within Gesher's runtime scope:
correct `REG_NEWLINE` matching is useful Android compatibility work, without
substituted inputs or application-specific behavior. However, Android's capture
results remain an unresolved correctness requirement after the ordinary repairs
recorded below.

The independently reviewed PR head was
`79beecf4e91aabf953aa4973b3a296b1213025d8` in
`ztcebuck/gesher:fix/bionic-regex-newline-escapes`. GitHub was checked again after
QA: open, not a draft, and mergeable, at the same head. This is also the commit
named in the earlier review; a later explanatory comment did not change it.
Local integration included a clean merge of main
`7b50854206a2b3e8f7b4266a132285d6449546f6`. There were no textual conflicts.

## Android reference

The reference is the exact `android-16.0.0_r4` AOSP source, not glibc's `regex`
or the PR's own test expectations:

- [Android libc build configuration](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/Android.bp), `libc_netbsd` (`POSIX_MISTAKE`, without `NLS`).
- [NetBSD compatibility header](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-netbsd/android/include/netbsd-compat.h).
- [regcomp.c](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-netbsd/lib/libc/regex/regcomp.c), particularly `p_re`, `may_escape`, `p_bracket`, and per-group `pend` checks.
- [engine.c](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-netbsd/lib/libc/regex/engine.c), particularly `dissect` and `backref`.
- [regexec.c](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-netbsd/lib/libc/regex/regexec.c).
- [regex.h](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/regex.h).
- [cname.h](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-netbsd/lib/libc/regex/cname.h), retained with its BSD license in `compat/bionic/posix_regex_cnames.h`.

The downloaded `regcomp.c` SHA-256 was
`8308c820bff17d4b1e33e2a28b6c1303aede890e6b82b63f8a584e9f10f3dc82`;
`engine.c` was
`c541eb37440b60fa7ffd74ca7bfd7deb5b482d945a9dd31893296e6da1e41ae1`.
The three compile/execute/free C sources were built as a host reference library
with `-DPOSIX_MISTAKE`, no `NLS`, renamed exported symbols, and compatibility
macros for NetBSD/Android annotations. The behavioral comparisons used the C
locale. Matching and parsing code in the reference files was not modified.
This is a host execution of pinned source, not a claim of device execution.

## Ordinary repairs made during QA

- Preserve the compiler's actual entry instruction. Alternatives and nullable
  leading expressions do not necessarily begin at instruction zero.
- Keep internal capture instructions when `REG_NOSUB` is set; it suppresses
  public output, not the values required by BRE backreferences.
- Track each of the first nine groups' closure separately, so closing an inner
  group does not make its still-open outer group a valid backreference.
- Replace per-transition native recursion in backreference matching with
  explicit heap-owned continuations, repeated-state detection, and bounded
  instruction, comparison, and working-state accounting. Bound compiler nesting
  and NFA working-state allocation, returning `REG_ESPACE` on resource failure.
- Preserve the current NFA state lookup when advancing input, so an injected
  fresh start deduplicates against already active states. Handle visitation
  epoch wrap explicitly.
- Own the compiled program with RAII until compilation succeeds.
- Match AOSP's exact-byte BRE backreference comparison under `REG_ICASE`.
- Reject empty expressions/alternatives and unsupported alphabetic escapes
  with the AOSP error values; retain valid empty groups and punctuation/LF
  escapes. Correct selected BRE repetition/parenthesis error handling.
- Implement the AOSP named collating elements and bracket word boundaries,
  and align bracket range and malformed-class errors.
- Correct the test that accepted `(foo|foobar)(bar|)`: AOSP rejects the empty
  alternative with `REG_EMPTY`. Use `(foo|foobar)(bar)?` for the valid capture
  preference test and explicitly test the invalid original expression.

These are partial repairs. Passing their regression tests does not resolve
all capture selection semantics.

## Remaining merge blocker: repeated-subexpression capture selection

Offsets below are half-open byte ranges. All examples are ordinary short
semantic cases; they do not require large inputs or resource exhaustion.

| Pattern and mode | Input | AOSP group 1 | Current group 1 |
| --- | --- | --- | --- |
| `(a\|aa)*`, ERE | `aaa` | `[2, 3)` | `[1, 3)` |
| `(aa\|a)*`, ERE | `aaa` | `[2, 3)` | `[1, 3)` |
| `(a*)*`, ERE | empty | `[0, 0)` | unmatched (`-1, -1`) |
| `(a*)*`, ERE | `a` | `[1, 1)` | `[0, 1)` |
| `\(a*\)*\1`, BRE | `aa` | `[2, 2)` | `[0, 1)` |

The current `BetterThread`, whole-match tie-breaker, and backreference
capture comparison prefer an earlier final capture start. That is insufficient
to reproduce AOSP's repeated-subexpression dissection. Epsilon visitation is
also keyed only by instruction within a closure, so distinct capture histories
can be discarded before comparison. Both issues affect public `regmatch_t`
results, even when the whole match is correct.

This needs a coherent capture reconstruction/tag-priority design, with coverage
for repeated and nested groups and empty matches. Reversing one comparator or
adding pattern-specific exceptions would not establish Android compatibility.
AOSP's `dissect` and `backref` behavior provides the reference. The remaining
change is substantial enough to require an implementation revision before
approval.

## Validation and limits

- The original exact-head production adapter and original host suite passed all
  38 checks. This independently demonstrates why its green tests were not
  sufficient evidence for approval.
- The repaired production adapter and expanded host suite passed all 64 checks,
  including the entry, flag, error, bracket, and capture ownership regressions.
- The expanded suite also passed AddressSanitizer and UndefinedBehaviorSanitizer
  with leak detection disabled for the managed host environment. Allocation
  ownership was reviewed separately; this is not a LeakSanitizer result.
- A fixed matrix of 1,596 bounded pattern/input/flag combinations was compared
  with the pinned source library. After the repairs, 1,566 agreed and 30 still
  differed, all in repeated-subexpression captures in that matrix. These counts
  are not a conformance percentage or a claim of exhaustive coverage.
- Guest native fixtures were expanded too, but Windows/ART execution is pending.
  This review did not run stack-overflow, resource-exhaustion, or other abuse
  demonstrations. Performance conclusions are limited to source analysis and
  the existing ordinary multiline host checks.
- This is not a certification of every locale or all BSD/GNU flag extensions.
  In particular, `REG_NOSPEC` and `REG_GNU` remain rejected by the adapter, as in
  its preceding implementation; they were not silently declared supported.

The repaired branch still must not receive an approval or automerge label until
the remaining capture behavior agrees with Android and current-head CI passes.
