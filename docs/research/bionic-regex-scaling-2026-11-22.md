# Bionic regex epsilon-closure reuse and boolean execution

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), N51 / T12,
reserved in [the round-two scope comment](https://github.com/gesher-runtime/gesher/issues/56#issuecomment-7082880912).
This is an execution-efficiency leaf, not a claim that the entire libc or regex
roadmap row is complete.

## Scope and Android contract

The change is confined to `Matcher::ExecuteNfa` and the corresponding matcher
flag in `compat/bionic/posix_regex.h`. The actual exported
`regcomp` / `regexec` / `regfree` wrappers remain unchanged. Applications still
receive the same match status, earliest match, longest whole-match span and
Android-compatible capture dissection. No application inputs, capabilities,
permission checks or workflows are substituted.

The pinned Android reference is Bionic commit
`f22516cbc67e81c13cf943ce02f01b8929a98726` (`android-16.0.0_r4`):

- [Public regex ABI and flags](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/include/regex.h).
- [Actual matcher](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/upstream-netbsd/lib/libc/regex/engine.c).
- [Bionic build configuration](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/Android.bp).

The Android matcher suppresses match outputs under `REG_NOSUB` and permits
returning after a verified match when no offsets are requested and the pattern
has no backreferences. Gesher now does the same in its existing NFA executor.
Requests with an observable whole-match span still continue through the longest
match. Backreferences keep their existing separate executor. `REG_STARTEND`
bounds remain inputs even when `nmatch == 0`, and boolean queries preserve every
caller-provided capture slot. Validation of absent/reversed bounds and the
existing `INT32_MAX` input-size limit happens before early success.

This is independently written production code under the project license. No
Android or other third-party implementation is copied into production. The
optional reference runner downloads eleven immutable, SHA-256-checked Android
source/header files into an external cache and retains their original notices.
They are compiled unchanged with host annotation macros, symbol renaming, the
C locale and Bionic's `POSIX_MISTAKE` setting. See the earlier
[regex QA report](bionic-regex-qa-2026-11-17.md) for the broader parity limits.

## Why the work can be shared

Previously every seed into an epsilon closure advanced a private epoch. A
nullable expression could revisit the same large subgraph once for each active
seed even though the closure at that byte position was already known.

The visit record now stores an input position and earliest originating start.
At the same instruction and input position, an earlier origin dominates a
later one: the future language is identical and this NFA phase does not carry
capture history. A later/equal origin can therefore reuse the earlier traversal.
If a better origin arrives after a worse one, it is allowed through and updates
all descendants. This retains the earliest whole-match choice regardless of
seed order. Capture reconstruction still runs separately over the chosen span.
The visit is also shared with the next newly injected unanchored start.

Visits store `position + 1`, which cannot wrap to the unvisited zero marker
because the public executor first enforces its existing signed 32-bit input
bound. The scratch can therefore be initialized entirely to zero. All
scratch remains per invocation; there is no persistent cache, shared ownership,
thread-local state or new synchronization. The visit record costs four extra
bytes per instruction, at most 256 KiB at the compiler's 65,536-instruction cap.
That storage is explicitly charged against the unchanged 64 MiB work budget,
including the simultaneously live capture-dissection scratch.

For a single anchored origin, each instruction is expanded at most once per
input position. The fixture enforces this bound on production code. The change
does not claim that every unanchored pattern has that bound: an earlier origin
can require a second traversal after a later origin reached a state first.

## Actual production work and timing

Baseline: `bf7c6e2f1adac284bd00971734f7b50c63d72a94`. Both binaries link the
complete production `syscall_bionic.cc`, including the real symbol-registration
and wrapper path. Only three unused external raw-syscall boundaries have inert
fixture definitions; no regex behavior is mocked. The checked-in
[JSON receipt](bionic-regex-scaling-2026-11-22.json) records source, fixture and
runner hashes, compiler identity, all work counters and all 120 timing samples.

Work counts come from a separate build that inserts only three counters into
the actual production NFA body. Normal correctness and timed builds use the
unmodified source bytes. The runner rejects missing or ambiguous insertion
anchors. The old production binary fails exactly four new work controls; a
no-op optimization cannot satisfy the improvement checks.

| Workload | Expanded states before | Expanded states after | Reduction |
| --- | ---: | ---: | ---: |
| Anchored nullable hit, `^(a?){128}a{128}$` | 3,553,988 | 99,588 | 97.2% |
| Anchored nullable miss, `^(a?){128}b$` | 3,537,346 | 82,946 | 97.7% |
| Unanchored nullable search, `(a?){32}a{32}b` | 1,271,906 | 96,354 | 92.4% |
| Boolean `a.*`, zero match count | 1,835,016 | 8 | 99.9996% |
| Boolean `a.*`, `REG_NOSUB` | 1,835,016 | 8 | 99.9996% |
| Observable whole span, `a.*` | 1,835,016 | 1,835,016 | 0% |
| Ordinary anchored hit | 83 | 83 | 0% |
| Ordinary anchored miss | 24 | 24 | 0% |
| Literal search | 4,115 | 4,115 | 0% |
| Captured key/value | 143 | 134 | 6.3% |
| Repeated captures, `(a|aa)*` | 3,451 | 2,054 | 40.5% |

For each boolean workload, consumed input falls from 262,145 bytes to one byte.
The `nmatch == 1` control still consumes all 262,145 bytes and returns the full
longest span. Backreferences use the separate executor and record zero NFA
counts in both builds.

The following are medians of five alternating baseline/candidate process-CPU
timings, at least 80 ms per retained sample, with compiled expressions reused.
Compiler: GCC 14.2.0, `-O2`, C++20. Host: Linux x86-64, AMD EPYC 9V74, shared
execution environment. Measurements are informational; deterministic state
counts are the CI regression criterion.

| Workload | Before, ns/call | After, ns/call | Before / after |
| --- | ---: | ---: | ---: |
| Nullable hit | 11,441,000 | 404,941 | 28.25× |
| Nullable miss | 12,677,625 | 350,715 | 36.15× |
| Nullable search | 4,484,219 | 451,375 | 9.93× |
| Boolean zero count | 9,388,875 | 137.525 | 68,270× |
| Boolean `REG_NOSUB` | 8,321,000 | 125.876 | 66,105× |
| Observable long span | 8,239,000 | 8,408,000 | 0.980× |
| Ordinary hit | 618.103 | 667.206 | 0.926× |
| Ordinary miss | 256.535 | 305.737 | 0.839× |
| Literal search | 41,062.012 | 40,393.555 | 1.017× |
| Captures | 1,660.446 | 1,587.845 | 1.046× |
| Repeated captures | 30,440.918 | 25,841.309 | 1.178× |
| Backreference | 451.374 | 427.368 | 1.056× |

The final timing run therefore has regressions: 7.9% for the ordinary hit,
19.2% for the ordinary miss (both about 49 ns), and 2.1% for the observable long
span. The larger visit record and its bookkeeping are a tradeoff; these results
do not establish a universal regex speedup. Zero initialization was selected
after a separate paired comparison with a nonzero sentinel showed lower costs
for several short cases; there is no pattern-size threshold or second executor.
Boolean timing uses `REG_STARTEND`; ordinary
NUL-terminated input still requires the unchanged wrapper to find the string
length. The very large bounded-boolean ratios are not an end-to-end application
throughput claim.

## Validation and CI execution

- 82,944 deterministic cases cover 36 BRE/ERE expressions, case folding,
  newlines, anchors, empty matches, bounded subranges, embedded NUL, ambiguous
  alternatives, repeated captures, backreferences and match counts 0/1/2/4/8.
  Both candidate and historical production agree with the pinned Android C
  implementation: **205,154 checks, zero failures per binary**. Both produce
  fingerprint `1128818218101492231`.
- ASan/UBSan runs the existing ABI suite plus the new checks: **113 ABI check
  lines, 121,994 host checks and 38 work checks**, with zero failures. A guard
  page follows bounded inputs. A separate inaccessible, uncommitted 2 GiB
  mapping proves nullable boolean queries still return the pre-existing
  `REG_ESPACE` size error before reading input. This last control tests Gesher's
  existing resource bound and is not presented as Android size-limit parity.
- The shared public ABI fixture has **22 checks** and is included by both the
  existing host raw-syscall test and `libc_test.c`. The latter is compiled by
  both x86-64 and ARM64 NDK guest workflows. The same C fixture was compiled and
  executed against the pinned Android sources: **22 checks, zero failures**.
- `tools/test_raw_syscall_contracts.py`, already called by the full CI workflow,
  now invokes the scaling helper and propagates any failure. Default CI needs
  neither a historical checkout nor a network oracle download. The optional
  Android and before/after measurements are additional reproducible evidence.
- Local ASan/UBSan execution explicitly sets `ASAN_OPTIONS=detect_leaks=0`:
  LeakSanitizer cannot initialize under this workspace's ptrace environment.
  Address and undefined-behavior sanitizers remain enabled, and the CI entry
  point does not set that exclusion.

No Windows/ART execution or physical Android device run is claimed by this
report. Those remain the actual downstream CI jobs; compiling the exact Android
C implementation on Linux is a reference comparison, not a device receipt.
The seed corpus is substantial but finite, and it does not expand the existing
regex engine's locale, Unicode or syntax support.

## Reproduction

From a clean checkout containing this change:

```sh
python3 tools/test_raw_syscall_contracts.py --sanitize
python3 tools/test_regex_scaling.py \
  --compare-ref bf7c6e2f1adac284bd00971734f7b50c63d72a94 \
  --android-reference /tmp/gesher-android-regex-reference \
  --measure --output /tmp/gesher-regex-measurement.json
```

The reference command needs network only to populate its hash-verified cache.
The comparison also needs the named baseline commit in local Git history. A
default run is `python3 tools/test_regex_scaling.py`; it executes unchanged
production semantics and the deterministic work bounds without those optional
dependencies. On a ptraced host, apply the local LeakSanitizer exclusion noted
above rather than changing the CI sanitizer configuration.
