# Formatted input: reuse the locked stream owner

## Scope and result

This is a bounded N40/T12 efficiency leaf for [issue #56](https://github.com/gesher-runtime/gesher/issues/56).
`ScanLocal` and `ScanWideLocal` already pin the stream and hold both its CRT
lock and buffer-state mutex for the complete MinGW conversion. The actual
parsers nevertheless call public `getc`/`ungetc` or `getwc`/`ungetwc` for
individual characters, repeatedly looking up and locking the same owner.

The change keeps a thread-local pointer to the owner already held by that
scan. Same-stream callbacks use the existing buffer under those locks. The
ordinary owner path handles another stream, another thread, or a retired
owner. No conversion algorithm, application input, assignment, buffer
capacity, capability or public ABI changes. Formatting keeps its separate
existing context. This does not complete N40 or the broader roadmap.

For 64 scans of 65,536-character tokens, measured production registry visits,
state-mutex acquisitions and CRT-lock acquisitions each fall from **4,194,432
to 64**. In the declared Linux host boundary with the real pinned MinGW parser,
median CPU falls from **644.922 to 49.835 ms** for narrow input and **666.145 to
151.152 ms** for wide input. All eight measured workloads improve. These are
component results; actual Windows execution and performance remain separate
evidence requirements.

## Source and lifetime

The source baseline is `d38b5549dc3963f8d491e9205f9b2743bb6d3745`. Its complete
`stdio_output.cc` bytes are identical to the PR's current-main base,
`5aa6b39027e9c38de2781ff9cda066418d25b1ba`. Source hashes are:

| File | Baseline SHA-256 | Candidate SHA-256 |
| --- | --- | --- |
| `compat/posix/src/stdio_output.cc` | `7ed987581cbb4b8449293235d0b0fafa8fd6725b06820a97c23924bff949ba97` | `be072ca4a7d45cd6d7b89b60313c50b12a3553af86506a3f24fb573676277c5d` |

`ScanScope` is constructed only after successful `AccessStream` acquisition.
It uses the existing `CancellationCleanup`, so normal return, C++ unwinding
and the explicit Winpthreads cancellation chain restore the preceding pointer
before `AccessStream` releases the owner. Nested scans retain the outer
context. The callback requires the identical `FILE*` and a live pinned state;
if the stream is forgotten and re-registered during a nested operation, the
old state cannot supply buffered bytes from its retired lifetime.

The wide read/pushback conversion bodies are extracted into helpers without
changing their statements. Orientation, conversion state, EOF versus error,
pushback order and the reader's line-output flush remain in the same buffer
implementation. Public callbacks still route to the executable's shared
versioned stdio owner; the new context is installed and consumed there.

The added retained state is one thread-local pointer, plus a stack scope for
each active scan. There is no new heap allocation, retained input cache, worker
or timer. This report does not claim an RSS or whole-application improvement.
It does not repair the separate wide-state contracts under review in #73 or
implement the line-reader work in #337. A later merge with #337 must explicitly
compose the same-file source qualifications.

## Provider and host boundary

The pinned [`llvm-mingw` 20261006 build script](https://github.com/mstorsjo/llvm-mingw/blob/20261006/build-mingw-w64.sh)
selects MinGW revision `a3d93999ef0521681d45e445c39964a7af0f593f`.
`tools/test_stdio_scanning.py` downloads and verifies six immutable files from
that revision: the narrow and wide `sformat` cores and headers, and their
`vfscanf`/`vfwscanf` entries. The [narrow parser](https://github.com/mingw-w64/mingw-w64/blob/a3d93999ef0521681d45e445c39964a7af0f593f/mingw-w64-crt/stdio/mingw_sformat.c)
and [wide parser](https://github.com/mingw-w64/mingw-w64/blob/a3d93999ef0521681d45e445c39964a7af0f593f/mingw-w64-crt/stdio/mingw_swformat.c)
execute their actual conversion and lookahead logic. Downloaded sources retain
their original notices and are not copied into the repository.

The host compilation has a declared ABI adaptation: Windows' assignable
`va_list` becomes SysV `va_copy` with a cleanup that calls `va_end`. The helper
checks the exact source boundaries before applying it and marks generated
copies. Parser algorithms are unchanged; the generated source is not claimed
to be byte-identical to the provider. Raw and adapted hashes are recorded in
the adjacent JSON.

The complete production stdio adapter and the real parser callbacks execute
against real Linux files, pipes, descriptor operations and threads. Existing
host boundaries supply opaque `FILE` allocation, CRT recursive-lock tracking,
Win32 owner discovery and initialization. Linux provides its scalar ABI,
locale and numeric conversion functions. Optional callback hooks only schedule
nested operations, cancellation, exceptions and retirement before a real
public callback; they never manufacture input or conversion results.

Consequently the local corpus does not execute Windows UCRT/LLP64, the actual
Winpthreads explicit cancellation chain, cross-module owner discovery or the
Android ELF/ART path. Those remain required existing Windows/ELF CI gates.

One inherited parser difference is recorded rather than repaired: MinGW's
whitespace loop restores entry `errno` except for `EINTR`. With partial
nonblocking input, `%d %d` can therefore return its completed assignment and
set `ferror` while restoring `errno` to zero. The fixture checks those existing
results. A separate `%c` case, which does not enter that loop, requires the
actual `EAGAIN`/`EWOULDBLOCK` error and an untouched destination. This leaf
makes no broader errno-compatibility claim.

## Work and CPU measurements

The instrumented correctness build counts entries into production `Find`,
actual state-lock acquisition, and the existing host CRT-lock boundary. Each
row has the same counts for narrow and wide input. No per-character work
counter is inferred from a hand-written parser.

| Workload | Scan calls | Registry visits / state locks / CRT locks before | Each count after | Input consumed |
| --- | ---: | ---: | ---: | ---: |
| 65,536-character scanset and delimiter | 64 | 4,194,432 | 64 | 4,194,368 bytes |
| 16-character scanset and delimiter | 4,096 | 73,728 | 4,096 | 69,632 bytes |
| Integer, double, scanset, literals and `%n` | 16,384 | 409,600 | 16,384 | 262,144 bytes |
| Immediate integer mismatch | 32,768 | 163,840 | 32,768 | 0 bytes; original byte retained |

Timing uses GCC 14.2.0, `-O2`, Linux 6.18.57/glibc 2.41, on an Intel Xeon
Platinum 8573C host with nine CPUs in the allowed affinity set. Four independent
processes execute A/B/B/A, eight rounds per workload per process. All 256 raw
samples are retained. The table summarizes the 14 samples per revision after
excluding each process's first round. Input creation, open and seek are outside
the measured interval. Each conversion's values, destination contents and
character counts are verified inside it, equally in both revisions. Each
timing process passes 853,405 checks.

Registry/state instrumentation and sanitizers are disabled for timing. The
existing host CRT-boundary counter remains, as does that boundary's lock map
and heavier initialization model. Their relative cost differs from Windows'
CRT and completed `INIT_ONCE` fast path. These measurements substantiate this
host component improvement and must not be extrapolated to Windows speedups,
multithread throughput or end-to-end application CPU.

| Workload | Baseline median CPU, ms | Candidate median CPU, ms | Change |
| --- | ---: | ---: | ---: |
| Narrow long token | 644.922 | 49.835 | −92.27% |
| Wide long token | 666.145 | 151.152 | −77.31% |
| Narrow short token | 11.027 | 1.815 | −83.54% |
| Wide short token | 12.365 | 3.478 | −71.88% |
| Narrow mixed numeric record | 61.873 | 11.204 | −81.89% |
| Wide mixed numeric record | 70.784 | 18.145 | −74.37% |
| Narrow immediate mismatch | 26.378 | 8.268 | −68.65% |
| Wide immediate mismatch | 28.470 | 11.758 | −58.70% |

The raw JSON also retains per-sample elapsed time, source/provider hashes,
compiler/environment information, instrumented counts and negative controls.
These repeatable workloads do not establish a startup budget or characterize
every format, locale, storage device or contention pattern.

## Contract and regression evidence

- Candidate ASan/UBSan: **107,015 checks, zero failures**, using the complete
  production adapter and real pinned MinGW narrow/wide parser cores.
- Unchanged baseline with the same corpus and candidate work budget disabled:
  **106,987 checks, zero failures**. With the budget enabled it exits 1 on four
  named nested-scan registry/lock-budget assertions. Its excess acquisitions
  are an independently observed baseline, not a fabricated slow alternative.
- Shared Linux public reference: **95 checks, zero failures**. This is a libc
  reference check, not an Android or Windows runtime result.
- Six compiled production mutations each exit 1 on their intended assertion:
  missing narrow scope, missing wide scope, missing restoration, missing
  same-stream guard, missing live-state guard, and missing thread-local
  isolation. A crash, timeout or unrelated failure does not qualify a mutant.
- Tests cover exact assignments, `%n`, suppression, embedded NUL, refills,
  caller pushback, parser lookahead, mismatch, initial/partial EOF, real I/O
  errors and retry, UTF-8 decoding/truncation, nested same/other streams,
  same-`FILE*` retirement and re-registration, C++ unwinding, cancellation,
  another thread's lock exclusion and line-output flush before input.
- Existing stdio host suite: **269 core + 153 adapter + 71 Linux checks**,
  five cancellation phases, and exit-handler output ordering all pass. The
  existing generic glibc envelope explicitly lacks a wide scanner; the new
  actual-MinGW host fixture covers both widths, and the real Windows target
  now runs six cancellation phases, including both scan variants.
- The complete changed production translation unit and complete public
  `stdio_ofd_test.cc` compile for **Windows x64 and ARM64**, with the pinned
  `llvm-mingw-20261006-ucrt` toolchain and existing strict warning settings.
  This is compile evidence, not a locally executed Windows binary or full link.
- Independent media-agent source review and separate ASan/UBSan replay passed
  the same 107,015 + 95 checks and all six specific mutants.

Local sanitizer runs use `ASAN_OPTIONS=detect_leaks=0` because this executor's
ptrace restriction prevents LeakSanitizer inspection. CI's sanitizer settings
are unchanged. The existing host stdio workflow gains the new parser gate;
the public contracts are also called by the existing Windows `stdio_ofd_test`
and narrow Android `libc_test` fixtures. No assertion, timeout, runtime gate or
required provider is removed.

### Source qualification

The selected supplier policy pins stdio in its existing OFD dependency graph.
The original OFD manifest, patch, 71 digests and explicit false runtime approval
are unchanged. A new single-file transition records the complete before/after
hashes and an exact patch. The policy verifies the immutable manifest and patch,
reverses only that transition, compares the result to the actual baseline Git
bytes and digest, then reapplies it to reproduce the complete candidate bytes.
The existing OFD checks run on the restored source; all other qualifications
remain active.

The policy passes **32 transition controls**, four existing repinned-property
controls and **five new repinned-scan controls**. The latter include an
unrelated `State.fd` mutation outside the new code and an attempted false-to-true
runtime approval change; refreshing candidate hashes cannot authorize either.
Supplier metadata qualification still passes 36 semantic mutations and the
separate authority-hash negative. This source qualification supplies no actual
Windows/runtime approval.

The current main framework reference `fe42fc3d4854141df94cd1e05cfbd10c4a686315`
is retained. Its public framework workflow blob matches the new main recipe,
`6dc4cca565aaf15fdf518eee825b0e44765b275f`. The only workflow edit adds the parser
test to the existing stdio host step.

## Reproduction

Run from the repository root. The helper downloads and hash-checks the exact
parser files unless `--mingw-scanner-root` supplies an already verified copy.
The normal CI sanitizer command is:

```sh
python3 tools/test_stdio_scanning.py --sanitize --negative-controls
python3 tools/test_stdio_contracts.py --sanitize
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-selected-supplier.py
```

For the same unmodified baseline and timing builds:

```sh
python3 tools/test_stdio_scanning.py --source-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --sanitize --measure-only --output-dir /tmp/scan-baseline-contract
python3 tools/test_stdio_scanning.py --source-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --measure --compile-only --output-dir /tmp/scan-before
python3 tools/test_stdio_scanning.py --measure --compile-only --output-dir /tmp/scan-after
```

Execute `/tmp/scan-before/stdio-scanning timing`, then the candidate twice,
then the baseline again, from their respective output directories. Capture
each process's separate output; each emits 64 `SCAN_SAMPLE` JSON lines and a
checked summary. Summarize rounds 1–7 while retaining round 0. Running the
instrumented baseline executable with `gate` must reject the named nested
work budget. Full fork CI must separately build and execute the real Windows
targets and Android ELF/ART tests before runtime qualification is claimed.
