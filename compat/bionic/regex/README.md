# Pinned Android regex engine

These files adapt Bionic's NetBSD regex implementation at
[`android-16.0.0_r4`, commit `f22516cbc67e81c13cf943ce02f01b8929a98726`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/upstream-netbsd/lib/libc/regex/).
`upstream.json` records each original file's SHA256. All original BSD notices
remain in the source, and `tools/package/licenses/BIONIC-REGEX-NOTICES.txt` is
included by the installer. See the project's licensing decision, ADR 0001.

The reason to use this compiler/strip matcher is observable behavior. Bionic's
captured-expression prefilter and bounded backreference traversal have results
that a conventional AST backtracker does not reproduce. There are no pattern
or application exceptions. The contributor's original ABI/regression assertions
remain; additional controls cover the independently found gaps.

## Host adaptation

- The `.inc` files compile privately inside `posix_regex::netbsd` in the actual
  `syscall_bionic.cc` translation unit. Standard includes, libc aliases and C
  linkage scaffolding were removed; C allocations have explicit casts, one
  C variable named `try` is renamed, and shared macro definitions are restored
  after inclusion. Matching contexts are passed through hot paths so work and
  locale checks do not repeatedly resolve thread-local storage. The upstream
  parsing, strip and capture-selection structure remains.
- `POSIX_MISTAKE` is enabled, and `NLS` is disabled, as in the pinned Android
  libc build. Character classification and collation use Android's C rules,
  independent of the Windows CRT locale. `MB_CUR_MAX` comes from Gesher's real
  Bionic per-thread locale function. Isolated host fixtures supply that boundary.
- LP64 offsets and small-engine state words are explicitly 64-bit, including
  Windows LLP64. Native x86_64 uses signed plain-character promotion; native and
  translated ARM64 use unsigned promotion. The private `__gesher_arm64_regcomp`
  entry is selected by the translator's ordinary override registry. Its choice
  is stored in the compiled program and retained by subsequent execution.
- PEND/STARTEND operate on caller-provided byte spans without first finding a
  NUL terminator. Wrapper validation preserves invalid-span errors. Captures are
  staged and published only after success, including resource admission for all
  requested output slots. REG_NOSUB retains internal backreferences.

## Resources and concurrency

Compile and execution each own an arena capped at **64 MiB** of aggregate
allocated storage. The accounting includes every allocation header, retained
allocation, state/capture/last-position buffer, and both old/new blocks during
reallocation. Fixed-size `Program`/context objects and bounded stack frames are
separate; the input itself is borrowed. The former input-length capture-prefix
vector and custom backtracking-state copies no longer exist.

All upstream loops consume a shared operation budget. A straight strip scan
precharges its instruction count once; each backward epsilon edge charges its
revisited instructions before the jump. Other loop conditions charge directly.
A mandatory initial BOL without REG_NEWLINE/REG_NOTBOL and without backreferences
has one possible start: when captures are requested, one longest walk replaces
fast search plus the identical longest walk. This general opcode-level shortcut
preserves capture dissection, while avoiding duplicate matching work. Bounded byte
operations, comparisons, string scans, copies, clearing, allocations and output
publication also consume that budget. Compiler admission charges the pattern
length before converting strip lengths to uint32. Compilation and regular
matching allow **50,000,000** units; backreference execution allows **2,000,000**.
These are operation accounting limits, not wall-clock promises or an assertion
that backreferences have linear complexity. Recursive parser, repetition,
alternative-offset, dissection and backreference entries have a **256-frame**
limit. The upstream 100-empty-backreference recursion rule remains unchanged.

A resource failure returns `REG_ESPACE`, never a fabricated match or no-match.
All allocations are owned before engine code can throw; per-call arena cleanup
works even when C cleanup labels are bypassed. Compile failure does not publish
partial state. Work/locale/byte-ABI contexts are thread-local and restored by
scope; compiled expressions remain read-only during concurrent executions.

`tests/native/regex_budget_host_test.cc` injects small limits on ordinary short
inputs and wraps the actual arena allocator to check balance after failures.
These parameters are private host-fixture inputs; normal ABI entry points use
the documented defaults. `tools/test_raw_syscall_contracts.py --sanitize` runs
both the exported ABI checks and this resource fixture. These host checks and
Windows object compilation do not establish Windows/ART/product execution.
