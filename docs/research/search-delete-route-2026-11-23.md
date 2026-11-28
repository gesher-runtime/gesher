# Search deletion: retain the lookup route for rebalancing

This is scoped host evidence for issue #56. The complete 4,096-key deletion workloads reduce median process CPU by **37.49% for integer keys and 36.31% for common-prefix strings**. Successful delete/reinsert churn improves **30.37% / 23.32%**, and complete build/drain lifecycles improve **26.47% / 19.44%**. The same measured candidate costs **10.040 ns / 15.40%** on a missing integer key, with all six blocks adverse, and adds approximately 1 KiB to the static `Tdelete` frame. These costs are part of the proposal.

## Production change and ownership

The prior public `Tdelete` first searches iteratively to retain its required parent result, then searches the same route again recursively while removing and balancing. The candidate records pointers to the ancestor links during the first search in a bounded 128-slot local array. It removes the found node with the same successor routine, consumes those links in reverse order to rebalance, and publishes the final root. The original recursive removal remains the overflow fallback. No allocation, global state, cache, comparator adapter, or changed node representation is introduced.

The captured links are in strict ancestors, not the freed target/successor. Each lower link is consumed before any parent rotation can move its context. Non-root return values preserve the original parent address; the unspecified non-null root result is not dereferenced. Missing deletion preserves the tree. `Tsearch`, `Tfind`, `Twalk`, insertion, balancing, minimum removal and destruction retain their implementation. The successful path intentionally invokes the comparator fewer times; comparator side-effect counts are not claimed identical. Stable comparison order remains the required input contract. Same-tree read-only comparator reentrance and independent-tree deletion are tested; mutating the active tree from its comparator is not newly supported.

## Exact sources and ordinary build

- Baseline commit: `08146b1ea150229313ef14a8fcd99df217a57fb9`; complete source SHA-256 `1112a3c182072ece59b8b6c995267861a492e3e27e0b75214080855948b94260`.
- Candidate complete source SHA-256: `7a51b598404b9dbcbec399c0fee8b8221a9a0a4b44932ac4f99dec7e91913c3a`.
- One separately compiled ordinary caller object SHA-256: `dd3fa629f0adcfb7219ce2c7a1fb34e378510243d933bf354f809ea181149f4c`; the same object is linked with each complete production translation unit.
- Actual compiler: LLVM 23.1.3, commit `0d261d1ca552c95a8f007e061c787ac7132fbcbc`, Linux x86-64 host. Flags: `-std=gnu++20 -O3 -DNDEBUG -Wall -Wextra -Werror -fno-exceptions -fsized-deallocation -mno-ms-bitfields`. Timed objects have no sanitizer, LTO, allocation wrapper or comparison counter. The project `BIONIC_SYSV` declarations are retained.
- The archive retains full sources, headers, objects, executables, compiler/dependency/frame outputs, command arguments and actual stdout/stderr. A separately compiled counted caller links the same ordinary production objects; its durations are not used as performance evidence.

## Fixed measurement protocol

The final campaign ran **2026-11-23T20:07:57.363951+00:00–2026-11-23T20:10:27.056521+00:00**, after all four lanes explicitly stopped CPU work. It contains **648 fresh processes: 27 cases × six ABBA blocks × four processes**, with 1,296 first/steady records. Every case has 12 observations per implementation. No run was replaced or omitted. Medians below are ratios of the separately computed medians; all six paired-block ratios and all raw per-operation values are in the JSON.

Each workload verifies actual public results, key identity, its checksum and final tree state. Drain removes every key; churn removes and reinserts each key; lifecycle includes all insertions and deletion of the resulting tree. Mixed cases contain 25% or 75% successful deletion plus reinsertion and the remaining absent-key deletions. Integer and 72-byte-common-prefix string comparators are real callbacks. Setup is outside timing except in lifecycle; result checks and checksums are inside, while final exact-key validation and cleanup are outside. The denominator is workload key operations, so churn/lifecycle/mixed hits can contain multiple public function calls per operation.

Steady work uses `max(4, 524288 / size)` complete rounds; the first complete round is retained separately. First means the first workload batch after tree setup in a fresh process, not whole-application startup or an isolated cold deletion. The process CPU interval surrounds both monotonic wall-clock reads. With tiny trees each round is correspondingly tiny, so CPU includes substantial timer-boundary cost that the wall interval excludes. The two clocks must not be conflated or overhead-subtracted after the fact.

### All final cases

Negative percentages are improvements. CPU values are ns per workload key operation. First p95 is the R7 linearly interpolated sample p95 of only 12 fresh processes per variant, a descriptive observation rather than a tail guarantee.

| Workload | Keys | Kind | CPU before → after (ns/op) | CPU Δ | Wall Δ | First CPU Δ | First CPU p95 Δ |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| drain | 1 | int | 239.765 → 243.771 | +1.67% | -3.29% | -2.89% | -43.39% |
| drain | 7 | int | 48.755 → 47.248 | -3.09% | -7.75% | -3.68% | -36.53% |
| drain | 16 | int | 38.934 → 39.998 | +2.73% | +0.19% | -10.08% | +0.33% |
| drain | 256 | int | 46.053 → 45.448 | -1.32% | -0.93% | -11.32% | -96.14% |
| drain | 4,096 | int | 213.341 → 133.355 | -37.49% | -37.56% | -33.06% | +11.44% |
| drain | 65,536 | int | 452.567 → 346.627 | -23.41% | -23.41% | -16.35% | -29.81% |
| drain | 1 | string | 245.902 → 235.696 | -4.15% | -14.96% | +3.51% | -1.20% |
| drain | 7 | string | 57.691 → 52.498 | -9.00% | -18.17% | -3.12% | -7.17% |
| drain | 16 | string | 52.141 → 49.829 | -4.43% | -7.83% | -8.21% | +3828.79% |
| drain | 256 | string | 76.076 → 68.264 | -10.27% | -10.24% | -24.92% | -38.37% |
| drain | 4,096 | string | 388.628 → 247.509 | -36.31% | -36.33% | -33.71% | -38.53% |
| drain | 65,536 | string | 780.581 → 594.028 | -23.90% | -23.56% | -22.68% | -29.97% |
| churn | 4,096 | int | 393.101 → 273.710 | -30.37% | -30.89% | -26.70% | -19.25% |
| churn | 4,096 | string | 639.458 → 490.356 | -23.32% | -23.58% | -26.28% | -66.81% |
| missing | 4,096 | int | 65.176 → 75.216 | +15.40% | +15.38% | -1.99% | +116.68% |
| missing | 4,096 | string | 216.914 → 211.244 | -2.61% | -2.70% | +15.05% | -16.28% |
| find | 4,096 | int | 56.956 → 55.527 | -2.51% | -2.70% | +16.43% | +18.98% |
| find | 4,096 | string | 197.274 → 180.681 | -8.41% | -8.13% | -6.53% | -11.41% |
| lifecycle | 4,096 | int | 321.536 → 236.431 | -26.47% | -26.21% | -19.14% | -23.49% |
| lifecycle | 4,096 | string | 593.439 → 478.062 | -19.44% | -19.56% | -15.42% | -4.86% |
| duplicate | 4,096 | int | 134.352 → 135.257 | +0.67% | +0.52% | +0.10% | +6.68% |
| duplicate | 4,096 | string | 240.073 → 249.504 | +3.93% | +3.91% | -0.62% | -25.81% |
| mixed25 | 4,096 | int | 137.816 → 116.426 | -15.52% | -16.22% | -11.71% | -27.71% |
| mixed25 | 4,096 | string | 339.202 → 289.696 | -14.59% | -14.55% | -18.43% | -23.97% |
| mixed75 | 4,096 | int | 292.002 → 210.793 | -27.81% | -26.97% | -33.97% | -20.38% |
| mixed75 | 4,096 | string | 538.097 → 433.495 | -19.44% | -19.48% | -19.33% | -39.76% |
| empty | 4,096 | int | 2.973 → 3.341 | +12.38% | +11.88% | -4.49% | +68.07% |

Large 4,096/65,536-key drains, both churn cases and both hit/miss mixtures improve in all six paired blocks. Integer lifecycle has one +0.65% adverse block among five improvements; its median gain is not a claim that every run improved. The 4,096 integer missing-key regression occurs in all six blocks (+1.31% to +29.71%). String misses have mixed-sign paired results despite their median −2.61%; their first CPU/wall medians worsen +15.05%/+19.43%.

Other retained adverse controls include one-key integer drain CPU +1.67%, 16-key integer drain +2.73%, empty deletion +0.368 ns/+12.38%, and unchanged duplicate string insertion +9.431 ns/+3.93% (five of six paired blocks adverse). Unchanged integer find first CPU/wall worsens +16.43%/+17.04%. `Tfind` and duplicate insertion are unchanged control implementations: neither their favorable nor adverse shifts are attributed to removal of the second comparator pass. All first p95 observations, including unfavorable ones, are present above and in the JSON.

## Comparison and resource accounting

The 54 separate counted processes cover all 27 cases and both implementations. Each counts its first and steady whole batch, twice the stated key count. Both production objects are identical to the ordinary timed build; only the separate caller adds comparator/allocation counters. The following integer results count complete actual public work:

| Workload (4,096 keys × two batches) | Comparisons before → after | malloc calls / bytes, both | frees, both |
| --- | ---: | ---: | ---: |
| drain | 164,328 → 82,164 | 0 / 0 | 8,192 |
| churn | 275,120 → 185,964 | 8,192 / 262,144 | 8,192 |
| missing | 98,308 → 98,308 | 0 / 0 | 0 |
| lifecycle | 254,442 → 172,278 | 8,192 / 262,144 | 8,192 |
| mixed25 | 145,416 → 120,840 | 2,048 / 65,536 | 2,048 |
| mixed75 | 229,236 → 163,922 | 6,144 / 196,608 | 6,144 |
| empty | 0 → 0 | 0 / 0 | 0 |

Successful deletion allocates nothing and frees exactly one node; an absent key allocates/frees nothing. Caller-owned key pointers retain ownership. Full drain halves comparator calls exactly; churn and mixed modes also include unchanged insertion comparisons. No dynamic-memory saving or complete-application gain is inferred from those call counts.

| `Tdelete` static function frame | Before | After |
| --- | ---: | ---: |
| Linux x86-64 / LLVM 23 | 40 B | 1,096 B |
| Windows x86-64, project SysV ABI | 72 B | 1,128 B |
| Windows ARM64 | 64 B | 1,136 B |

These are actual compiler `.su` static function frames, not the complete recursive peak stack or a runtime stack measurement. The additional local route also affects misses, tiny trees and nested calls. The candidate uses no retained heap buffer. A full 128-level valid AVL tree is not physically realizable with available address space; the fixture reduces only the route capacity to two in the same full production source to execute overflow and fallback on valid trees. That exact fallback preserves the complete baseline/candidate public traversal trace, and a compiled off-by-one mutant is rejected by ASan. This is not a claim that an actual 128/129-depth valid tree was constructed.

## Public semantics, actual Android source and negative controls

The shared public-API corpus runs 3,654,000 checks per implementation: sorted/reversed/shuffled insertions, root/extreme/shuffled deletion, parent return, duplicate identity, missing keys, callback errno, remaining keys, complete VISIT/order/depth/balance and exact remaining-key destruction. Baseline, candidate and two-slot fallback produce the identical complete trace `cb5909ac8b767a1e`. The fixture is included by the existing native guest test and is also valid strict C17.

The complete actual production translation unit is linked without extracting or recreating the algorithm. Additional host tests exercise comparator reentrance with read-only access to the active tree, deletion in another tree, eight independent concurrent trees, and allocation denial around the actual deletion call. Host counts are 101,442 candidate, 102,019 baseline, 101,968 two-slot fallback and 100,992 supplier checks. All have the same 3,654,000 shared checks.

The test-only supplier is the unmodified Android Bionic `tsearch.c`, `tfind.c` and `tdestroy.cpp` family selected by `libc/Android.bp` at [android-16.0.0_r4](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/), tag object `2ae67d60dd95c0f0c8fe12c30907be36f2ac17ff`, peeled commit `f22516cbc67e81c13cf943ce02f01b8929a98726`. Sources, exact Gitiles receipts, SHA-256 pins and original Public Domain/Apache-2.0 notices are retained under `tests/native/reference/search` and in the archive. Public semantics are compared against the actual supplier; its valid unbalanced shape is not forced to match the AVL implementation. Its distinct complete trace is `317c81fece2db47e`. This is the Bionic repository, separate from the external LLVM libc supplier used in the earlier Memrchr report.

GCC 14 ASan/UBSan passes baseline/candidate/supplier/fallback and six genuinely compiled negative controls: omitted ancestor balancing, wrong parent result, missing-key false success, lost successor key, omitted overflow fallback, and route off-by-one. Every negative binary compiles and fails its relevant runtime contract. Ordinary LLVM 23 independently passes all four positives and the five semantic negatives; the out-of-bounds negative is restricted to the sanitizer run. A separate strict C17 caller passes the supplier's complete shared corpus. The first LLVM driver attempt failed after its two production positives because a compiler-name parser constructed `clangcc`; that error and all outputs remain archived, followed by the corrected complete attempt. No source predicate was weakened.

Local ASan runs set `ASAN_OPTIONS=detect_leaks=0` only because this workspace's ptrace environment cannot run LeakSanitizer. The committed CI command does not disable LeakSanitizer. Explicit allocator ownership checks remain active locally. Complete production TUs cross-compile with strict ordinary flags for Windows x86-64 and ARM64. Those object builds and Linux host/supplier execution do not establish Windows runtime, an Android-device result or the complete issue budget. No Android NDK/sysroot was installed locally; the existing full CI builds and runs its guest/runtime surfaces.

## Earlier experiments are retained

The first recursive one-pass removal prototype ran a fixed 64-process pilot and was rejected: despite large successful-deletion gains, integer misses worsened +79.22%. A separately retained longer 96-process ABCCBA comparison of baseline, rejected recursive candidate and this bounded-route candidate found the recursive miss +102.20%; the route miss was +9.60% integer/+12.38% string. Both pilots retain complete raw first/steady observations, binaries, caller/source identities and their noisy unchanged controls. The final campaign keeps the route production source unchanged, uses the common enum-dispatch caller and ordinary project flags, and broadens to 27 cases. Measurements across different callers/flags are not pooled. This final fixed campaign was not repeated to select favorable results.

Independent review checks all 648 actual process outputs, fixed block order, build/binary/source hashes, 27 × four metrics and paired ratios: 8,706 checks. The final historical report validator independently verifies the archive and recomputes the public summary. Current production semantics remain a separate executable CI gate; the historical evidence gate does not freeze unrelated future tree implementation changes or label them as the old measured source.

## Reproduce qualification and inspect evidence

```sh
python3 tools/test_search_delete_contracts.py --sanitize --negative-controls
python3 tools/test_search_delete_report.py
```

For a new experiment, `tools/measure_search_delete.py --prepare --output-dir NEW_DIRECTORY --baseline COMPLETE_BASELINE.cc --compiler clang++ --cross-bin LLVM_MINGW_BIN` freezes all ordinary inputs. After qualification and explicit idle coordination, `--run --output-dir NEW_DIRECTORY --quiet-confirmed UTC_TIMESTAMP` runs exactly six ABBA blocks. The driver refuses to overwrite an existing measurement. New measurements are separate evidence, not edits to this historical result.

Machine-readable complete results and member manifest: `search-delete-route-2026-11-23.json`. All raw observations, both pilots, source/supplier files, full positive/negative binaries, failed driver attempt and independent review: `search-delete-route-2026-11-23.tar.gz`, **448 regular members**, **3,049,696 bytes**, SHA-256 `57ac04ff0314a9ed71c34ffe473e2b47c53c8d41c0053547981abec62feefa2f`.
