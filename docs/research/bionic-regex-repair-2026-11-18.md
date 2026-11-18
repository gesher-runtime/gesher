# PR60: bounded Bionic regex repair — 2026-11-18

This implementation addresses the independent review of contributor head
`8f46c2f6f8ccee8b9e20cc6a0d8ecff3fbd8f607`. The contributor's commits and original
ABI assertions are retained. Main `6179f743f9c92381e292a8e87e2c96f9a5cd1fbc` is
integrated. This is an implementation record for subsequent independent QA,
not a claim of reviewer approval or Windows/ART execution.

## What changed and why

The earlier custom compiler correctly improved ordinary newline matching, but
its word-boundary handling and BRE backreference representation differed from
Android. In particular, Bionic re-evaluates the captured expression's strip in
its initial search, and bounded backreference traversal has observable quirks
that an ordinary AST backtracker does not reproduce. The current implementation
adapts the pinned, permissively licensed
[Bionic/NetBSD source at android-16.0.0_r4](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/upstream-netbsd/lib/libc/regex/).

The exported wrapper now preserves artificial slice beginnings, repetition of
escaped word boundaries, captured anchors in backreference filtering, and
Bionic's bounded-backreference capture/result selection. Native x86_64 and
native/translated ARM64 use their respective plain-character signedness;
Bionic's actual per-thread locale selects the engine's byte-locale behavior.
No Windows CRT locale or application/pattern exception selects a result.

All dynamic compiler/execution storage is owned in bounded arenas, including
headers, retained capacity and simultaneous old/new reallocation blocks. Work
is charged for prefilters, instruction scans, dissection, backtracking and bulk
operations. Per-call state unwinds on resource failures and captures are only
published on success. The [adaptation notes](../../compat/bionic/regex/README.md)
state the exact 64 MiB memory, operation and recursion limits. Resource limits
return `REG_ESPACE`; they do not turn failure into success or `REG_NOMATCH`.

## Implementation validation

- **106 exported production-wrapper assertions pass under ASan/UBSan**, including
  every submitted assertion, the review counterexamples and byte-ABI controls.
- **26 resource and ownership checks pass under ASan/UBSan.** They use ordinary
  short input and small injected private limits, with actual allocator balance
  checked after failed compilation, prefiltering, capture allocation, recursion
  and reallocation. Output nonmutation, later successful calls and concurrent
  independent budgets are covered. Disabling engine work charges in an isolated
  negative-control copy makes the prefilter-budget assertion fail.
- **19,180 bounded comparisons agree with the pinned source oracle.** For each
  combination of signed x86_64 / unsigned ARM64 bytes and C / UTF-8 locale,
  this includes the independent unchanged 1,907-case matrix, 512 additional
  high-byte parsing controls and 2,376 leading-anchor controls. The oracle is
  unmodified Android source with host allocation/ctype infrastructure, not
  glibc's regex implementation or a full Android runtime.
- Strict LLVM-MinGW compilation passes for the production syscall/regex
  translation unit on **x86_64 and AArch64**, and for the x86_64-hosted translator
  override translation unit. The extended native x86_64 guest fixture compiles
  with NDK r27c / API35. These are object checks, not product-link or runtime
  qualification.
- The original independent review also retained actual NDK r27c static-Bionic
  controls under QEMU on both guest architectures. Those are older-Bionic
  corroboration for the ASCII semantics and byte-ABI differences, not an
  Android16 device or Gesher execution claim.

LeakSanitizer was disabled for the container's ptrace limitation; allocator
balance and ASan/UBSan are separate evidence, not a LeakSanitizer pass. The
unchanged `regerror` name/number conversion limitations are not repaired here.

## Performance and resource tradeoff

The first adaptation spent too much time resolving its thread-local context in
instruction loops. The current code passes the context through hot matcher
calls and precharges straight instruction scans. A mandatory leading BOL with
one legal starting position also avoids a duplicate whole-match search. This
is a generic opcode property; it does not recognize particular pattern text.

A small Linux x86_64 `-O2` comparison used the actual registered old/new wrappers,
compiled each pattern once and reported medians of three sequential batches:

| Ordinary control | Original PR head | Repaired implementation | Ratio |
| --- | ---: | ---: | ---: |
| 800-line newline no-match, 1,600 input bytes | 11.96 μs | 0.94 μs | 0.08× |
| Anchored positive match, 1,000 input bytes | 18.02 μs | 23.81 μs | 1.32× |
| Capture selection, 8 input bytes | 5.63 μs | 10.88 μs | 1.93× |
| Backreference no-match, 6 input bytes | 1.23 μs | 1.22 μs | 1.00× |

The repaired engine is **not uniformly faster**. The capture sample still has
roughly 5 μs additional host cost, and the long-positive sample roughly 6 μs;
these tradeoffs accompany complete operation/storage accounting and the
reference's capture machinery. Shared-container microbenchmarks are noisy and
do not establish application throughput or Windows latency. The submitted
100/200/400/800-line scaling regression also passes. No broad linear-time
backreference or capture-dissection claim is made.

## Reproduction

Run `python3 tools/test_raw_syscall_contracts.py --sanitize` on a host with a
C++20 compiler. The script hashes all imported production engine sources and
runs the ABI and resource fixtures. On this container it was invoked with
`ASAN_OPTIONS=detect_leaks=0` for the stated limitation. Native Windows/ART,
translated guest execution in Gesher, final linking and installer execution
remain to be run in the appropriate environment. The BSD notices are included
in both source and the installer inputs.
