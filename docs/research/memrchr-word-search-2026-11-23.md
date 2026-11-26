# Bounded reverse-byte search: component evidence

Related: [issue 56](https://github.com/gesher-runtime/gesher/issues/56).
The accompanying `memrchr-word-search-2026-11-23.json` retains all raw processes,
earlier adverse variants, source/reference receipts, compiled controls and the
independent reviews. `tools/test_memrchr_report.py` verifies its archive and
recomputes every median, absolute delta and six paired-block results.

## Change and scope

The existing Bionic `memrchr` shim examines one byte per iteration, even when a
long range contains no match. The selected implementation retains a scalar
path for ranges shorter than 16 bytes. For longer ranges it first examines
the last eight bytes, then tail-calls a private word-search function. Both
functions retain `BIONIC_SYSV`; the public export and checked/FORTIFY wrapper
are unchanged.

Each word load covers eight complete bytes inside the supplied range, using
`memcpy` for legal unaligned access. The unsigned zero-byte expression is only
a block predicate. Subtraction can mark a neighboring nonmatching byte, so a
matching block is always checked byte by byte in descending order. The final
remainder retains a bounded scalar scan. There is no heap allocation, cache,
global state, feature detection, alignment precondition or read outside the
caller's range. Complexity remains linear; the long miss path does less work
per byte.

This is measured component work on an ordinary Linux host. It does not prove
Windows/ART execution, an application speedup, ARM64 performance, or closure
of the issue's broader Android/Windows acceptance rows. The full production
Windows translation units are separately compiled and source-bound below.

## Fixed measurement

The exact comparison baseline is main
`e505ad227523aaca566d05b8af6e466495c819cc`. Production source SHA-256 is
`feefc376c3f19b9fe9de0266f0a24da2ad179af0360000874235da54cd8562ab`.

`tools/measure_memrchr.py` compiles the actual unchanged/candidate functions,
including the actual checked wrapper and fatal path, into separate ordinary
objects. The caller is compiled once and linked unchanged to each object.
There is no LTO, counter instrumentation, optimizer model or helper-only
timing. The extraction erases `BIONIC_SYSV` only to use the native Linux calling
convention; the separate Windows build keeps the real guest ABI declarations.

Both final campaigns use the same LLVM 23.1.3 frontend as the Windows
toolchain, C++20, `-O3 -DNDEBUG -Wall -Wextra -Werror -fno-exceptions`, and caller
object SHA-256 `94b4c2898edecdbb29e015b3181a84cce5195b5e2230f81bfa2086f0cdc57460`.
The selected campaign ran **18:00:01.500110–18:00:13.723174 UTC** on
23 November 2026, with all other agent CPU work explicitly paused.

All 30 declared cases use six ABBA blocks, with case order rotated by seven
between blocks: **720 fresh processes**, 12 processes per variant per case,
and **968,656,080 complete first/steady calls**. Each process retains its first
call and one steady batch. Every returned pointer is checked against a
forward-scan oracle prepared before timing; return checks and checksum work
remain inside the steady measurement. Input creation and oracle construction
are outside it. Small/near-end cases use two million calls; long scans target
64 MiB of input per process. Mixed ranges contain 32 fixed inputs spanning
7, 17, 256 and 4,096 bytes with miss, first, middle and last matches.

Process CPU and monotonic wall time are both retained. First-call values
include clock/probe overhead and are especially noisy for tiny operations;
sub-microsecond first-call percentages are not release budgets. Peak RSS is
whole-process fixture/input high-water memory, not a production allocation
counter. Every process, first sample, control and adverse result is retained;
there is no outlier deletion or best-run selection.

## All selected-case results

CPU columns are median nanoseconds per complete call, including caller
validation. Negative changes mean lower cost. The last column is the absolute
change in median first-call wall time; the JSON also retains all six paired
block changes, raw CPU/wall observations and RSS values.

| Case | Baseline CPU ns | Candidate CPU ns | CPU change | Wall change | First wall delta ns |
|---|---:|---:|---:|---:|---:|
| `empty` | 2.611 | 3.171 | +21.46% | +21.23% | -12.0 |
| `one_miss` | 1.909 | 2.281 | +19.50% | +19.38% | -18.0 |
| `one_hit` | 2.738 | 2.666 | -2.63% | -1.50% | +11.5 |
| `tiny7_miss` | 5.130 | 5.194 | +1.25% | +1.27% | +1.5 |
| `tiny8_miss` | 5.011 | 5.603 | +11.82% | +11.77% | -3.5 |
| `tiny9_miss` | 6.344 | 6.477 | +2.09% | +1.28% | +2.5 |
| `tiny15_miss` | 10.166 | 8.147 | -19.86% | -20.36% | +4.5 |
| `word16_miss` | 10.460 | 8.118 | -22.39% | -22.30% | +74.0 |
| `word17_miss` | 8.162 | 8.150 | -0.14% | -0.15% | +87.0 |
| `word31_miss` | 13.723 | 9.299 | -32.24% | -31.31% | +87.0 |
| `word32_miss` | 14.194 | 9.249 | -34.83% | -34.84% | +99.0 |
| `word32_first` | 14.451 | 8.012 | -44.56% | -44.56% | +68.0 |
| `word32_last` | 2.691 | 2.520 | -6.35% | -6.35% | -1.5 |
| `word32_penultimate` | 2.672 | 2.580 | -3.45% | -3.45% | -17.0 |
| `word32_frequent` | 4.405 | 3.075 | -30.20% | -29.74% | -11.0 |
| `block256_miss` | 156.338 | 24.496 | -84.33% | -84.41% | +16.5 |
| `page_miss` | 2,071.264 | 338.776 | -83.64% | -83.66% | -1,321.0 |
| `page_first` | 2,049.344 | 350.067 | -82.92% | -82.93% | -1,334.5 |
| `page_middle` | 1,043.857 | 180.290 | -82.73% | -83.00% | -609.5 |
| `page_last` | 2.456 | 2.366 | -3.63% | -3.70% | -28.5 |
| `page_penultimate` | 2.676 | 2.656 | -0.74% | -0.75% | -22.5 |
| `page_near_end` | 5.782 | 3.783 | -34.57% | -34.54% | +2.5 |
| `page_unaligned` | 2,095.718 | 360.508 | -82.80% | -82.79% | -1,313.5 |
| `mib_miss` | 510,232.594 | 82,534.812 | -83.82% | -83.87% | -410,193.0 |
| `mib_first` | 516,661.961 | 93,828.414 | -81.84% | -81.85% | -396,946.0 |
| `mib_middle` | 253,713.297 | 41,214.930 | -83.76% | -83.73% | -190,682.5 |
| `mib_last` | 2.648 | 2.597 | -1.91% | -1.91% | -13.0 |
| `mib_near_end` | 5.785 | 3.541 | -38.78% | -38.75% | +12.0 |
| `checked_page_miss` | 2,049.518 | 385.643 | -81.18% | -81.23% | -1,311.0 |
| `mixed_ranges` | 357.467 | 62.591 | -82.49% | -82.50% | +0.5 |


## Remaining costs and earlier designs

The long miss/first/middle, checked-page and mixed cases save 81.18–84.33%
median CPU. Every one of their six paired blocks improves. This is a substantial
reduction for those workloads, with no claim about their frequency in an app.

Tiny controls still have costs: empty +0.560 ns (+21.46%), one-byte miss
+0.372 ns (+19.50%), and eight-byte miss +0.592 ns (+11.82%). The one-byte miss
is adverse in all six paired blocks; it is not discarded as noise. A workload
dominated by those calls can regress. First-call wall cost also rises for some
word-sized cases, including 32-byte miss +99 ns. These limits prevent a universal
first-use or application-performance claim. Seven-byte miss is +0.064 ns
(+1.25%), nine-byte miss +0.133 ns (+2.09%), and near-end matches benefit from
the scalar suffix. All controls, including wide paired-block ranges, remain
in the JSON.

The original GCC 14.2 prototype retains all 112 processes at
17:05:22.706384–17:05:25.059311 UTC. It improved long scans but made the
seven-byte miss +37.59% (about 1.93 ns). It is a separate compiler/design trial.
The later inline-word Clang campaign retains all 720 processes at
17:45:33.993518–17:45:46.955104 UTC: seven-byte miss +32.99%, one-byte miss
+38.36%, and 32-byte penultimate match +33.13% were adverse.

Its actual generated function saves two registers before deciding whether
word scanning is needed, and the full Windows x64 object reports a 16-byte
public search frame. The selected private helper plus eight-byte suffix keeps
that word-loop register pressure out of the public scalar path. The two Clang
campaigns use the exact same caller object and inventory. Source/object hashes,
stack reports and disassembly are retained; these observations establish the
concrete generated-code difference, not sole causation of every timing delta.
There was one fixed final campaign after this change and no further tuning.

## Correctness and actual Android supplier

Both GCC ASan/UBSan and ordinary LLVM 23 Release checks pass **1,855,249 exact
calls per baseline/candidate/reference variant**: 1,769,478 shared calls plus
85,771 guarded calls. Shared cases cover all 256 bytes, all 16 alignments,
every match position across tiny/word boundaries, overflowing and negative int
arguments, repeated matches, subtraction-borrow patterns, 8,192 seeded binary
cases, checked calls, unchanged input and preserved errno. The host's real
inaccessible guard pages cover both ends of the range, including page-sized
inputs. The pre-existing shim's null/zero case is tested without turning it
into a promise of Android's nonnull public API.

The unchanged real FORTIFY wrapper aborts before an oversized search. Six
separately compiled mutants are rejected by their intended checks: ascending
block selection, treating borrow flags as exact positions, losing the remaining
first byte, omitting unsigned-byte conversion, underreading a guard page, and
removing FORTIFY rejection. This is executable production-function testing,
not a model of search or of the fatal path. The same C corpus is added to the
imported Android libc probe for actual platform CI.

Android 16 r4 x86_64 Bionic selects the actual
[external/llvm-libc supplier](https://android.googlesource.com/platform/external/llvm-libc/+/refs/tags/android-16.0.0_r4/src/string/memrchr.cpp).
The nine SHA-pinned source/header files compile unchanged with their real
namespace/public-packaging definitions. Supplier commit is
`24af38d6527d0ee6d61cea5052c83a01725ac98f`; source SHA-256 is
`1231a6c0d05ca8542cdff79e23becb1aa9c38e649cda4b5a31e2d6377f636dd5`.
The checked-call facade retains Gesher's actual FORTIFY; it is not presented
as Android FORTIFY execution. ARM64 uses a different Android assembly supplier,
so this x86_64 source comparison is not ARM64 reference execution.

An independent media agent reviewed range, effective-type, byte conversion,
last-match, guest-ABI and FORTIFY behavior, and replayed the complete sanitized
baseline/candidate/pinned-reference corpus plus all six mutants on the exact
production SHA. Its full receipt is archived.


## Windows and source qualification

The complete unchanged-baseline and selected-candidate `posix_bionic.cc`
translation units compile with the actual Windows x64 and ARM64 project
headers, definitions and guest calling convention, using LLVM 23 Release
warnings as errors. Each receipt records the exact command, output object and
23 project dependency digests. These are cross-compilation results; no Windows
execution was available locally.

| Function | Windows x64 baseline / selected | Windows ARM64 baseline / selected |
| --- | ---: | ---: |
| Public `Memrchr` stack bytes | 0 / 0 | 0 / 0 |
| Private word helper stack bytes | absent / 0 | absent / 0 |
| Checked wrapper stack bytes | 40 / 40 | 16 / 16 |

The earlier inline candidate's Windows x64 public search and checked wrapper
used 16 and 56 stack bytes respectively. All earlier objects are identified by
SHA and their stack/disassembly receipts remain in the archive.

The repository's selected TCP source policy pins this whole production file.
The original policy correctly rejects the changed source. An exact reversible
qualification patch reconstructs the original file before all 71 original
whole-file digest checks run. All old qualification files and receipts remain
unchanged. Original 26 checks plus four properties, shared 23 checks and 20 new
qualification checks pass, alongside the vector and supplier policies. An
independent reviewer repeated this verification, including byte-for-byte
recovery of the original policy. This grants no runtime or platform approval.

## Reproduction and retained evidence

From this tree, run:

```sh
python3 tools/test_memrchr_contracts.py --sanitize --negative-controls
python3 tools/test_memrchr_report.py
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
```

The reference manifest pins each of the nine actual upstream files. The
[Bionic source selection](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/Android.bp)
and [LLVM libc build definitions](https://android.googlesource.com/platform/external/llvm-libc/+/refs/tags/android-16.0.0_r4/Android.bp)
are retained with their source hashes. For a fresh host run, fetch those exact
manifest paths into a reference root and pass `--reference-root` to the contract
runner. The CI command exercises production contracts and negative controls;
the pinned supplier comparison is recorded in the retained local receipts.

This container's ptrace setup prevented LeakSanitizer startup, so the local
ASan/UBSan qualification used `ASAN_OPTIONS=detect_leaks=0`. The original failed
startup and an earlier mutant-fixture label correction remain archived. The CI
command does not disable leak detection. There is no allocation in the changed
production search.

Build separate baseline/candidate caller binaries and measure with
`python3 tools/measure_memrchr.py --help`; the archived build receipts contain
the exact compiler, definitions, sources, commands and caller/object hashes.
Use new output directories when reproducing. Timing is recorded per fresh
process; first calls and adverse controls are never replaced by a selected
steady-state sample.

The adjacent JSON contains the complete summaries and a deterministic
SHA-256-checked archive of 828 source, command, raw process, disassembly and
qualification receipts. `tools/test_memrchr_report.py` checks archive membership,
source identities, ABBA inventory and every summarized timing calculation.
Compiled executables are identified by digest rather than embedded. The
archive retains all three campaigns and independent reviews.

Real imported Android probe execution, both Windows architectures, full
workflow aggregates and representative application frequency remain separate
requirements. This leaf addresses one concrete search cost within #56; it does
not close the issue's broad application or platform rows.


## Current main composition

The publication is based on main
`d94ad6025ac79e2019026caa0bca6a4b9f8adca9`. Its 24 incoming paths relative
to the measured e505 baseline are retained exactly, apart from the two explicit
new CI commands. The complete baseline production file is identical on both
main snapshots. Every reviewed leaf source, contract, measurement fixture and
archived receipt remains byte-identical after composition. The JSON records
these identities and the actual passing selected, vector, supplier and report
gate outputs. No performance rerun or new platform claim is attached to this
nonoverlapping main update.
