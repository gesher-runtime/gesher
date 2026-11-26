# Stable sorting work in complete directory scans

## Draft consolidation on 2026-11-26

The measurements below retain their original archived sources and execution
receipts. They are not new measurements of the consolidated tree. The current
Scandir directory implementation is byte-for-byte the measured implementation;
other whole-file changes are admitted only through the exact reversible
`scandir-consolidation-source.json` and `.patch` transition to reviewed commit
`3a309b747fc604f84fa20a42bbd875d706116d6d`. The original supplier-CI adapter,
source pins, archive identity, statistics checks and negative controls still
execute after that transition. The verifier differs only in its two explicit
adapter-routing edits. The draft declaration keeps native runtime approval
false; complete Git-history policy checks and real Windows/ART validation
remain required before release approval.


23 November 2026. Refs #56 T12 and #178. Depends on published [#413](https://github.com/gesher-runtime/gesher/pull/413); this does not close a broad filesystem, Android-compatibility or release-performance requirement.

## Result and material costs

The fixed final comparison covers **360 fresh processes / 1,440 observations**, six ABBA blocks and 15 cases. Complete scan/copy/sort/output-validation/free CPU falls **64.31% at 128 reverse entries, 89.54% for 1,024 random entries, 94.59–94.99% for 1,024 reverse/lexical entries, 86.09% for tied keys and 98.27% for 4,096 reverse entries**. Every target improves in all six paired blocks. These are host component measurements, not Windows or application speedups.

**The common/control costs are part of the result.** The two-entry case is slower in all six blocks: 1.451→1.611 µs median CPU (**+160.3 ns/+11.05%**), paired mean +6.81% (descriptive 95% interval +1.83..12.67%). The 16-entry case has only +2.23% marginal median, but **+20.85% paired mean**, +20.74% pooled mean and +67.76% steady CPU p95; those expensive processes are retained. The already-sorted 1,024-entry first-call median rises 336.896→483.683 µs (**+146.787 µs/+43.57%**). The unsorted 4,096-entry first-call median rises 1,277.229→1,373.094 µs (**+95.865 µs/+7.51%**).

The first-call tails are not hidden behind medians: tiny2 CPU p95 is 5.745→51.237 µs; the 4,096-entry scan without sorting is 2,998.212→5,768.330 µs. There are only 12 first calls per variant/case. All absolute CPU/wall medians, p95s, maxima and individual observations are retained. **No universal 5% budget, cold-start equivalence or release acceptance is claimed.** No production tuning, replacement run or optional follow-up followed this campaign.

The implementation may allocate **one temporary 8N-byte pointer array**; it retains no extra heap after sorting. At 1,024 entries the measured scoped peak rises 294,912→303,104 B. Its compiler-reported Windows `ScanDirectory` static frame rises 600→664 B on x64 and 432→464 B on ARM64. These costs accompany the CPU gains.

## Change, ownership and compatibility

The existing code used adjacent insertion sort for every accepted entry. The new helper keeps that algorithm below 128 entries. Larger scans also begin with insertion sort, so already/nearly-sorted lists allocate no scratch. After 8N actual swaps, a stable bottom-up merge finishes the full list with one N-pointer scratch allocation. Left-on-equality selection and the already-stable insertion prefix preserve original enumeration order among equal keys, including a threshold crossing halfway through an insertion.

Every allocated directory record remains owned by the existing result array. Scratch contains borrowed pointers only, every merge pass fills its destination, odd tails are copied correctly, and a final copy-back occurs only when required. Scratch is freed before result publication. Failure of this optional allocation restores the comparator-entry errno and continues the original insertion sort; it does not add a new public ENOMEM result or retry the optional allocation. Existing count≤INT_MAX admission bounds pointer multiplication, merge widths and the post-failure swap counter on supported 64-bit hosts.

The four public `scandir`/`scandir64`/`scandirat`/`scandirat64` registrations, System V callback ABI, caller errno, filter/compare ownership, output-on-error contract, partial-prefix cleanup and fd ownership remain unchanged. Comparators receive local const-pointer variables as before. Comparator invocation count/order is intentionally reduced; no exact callback trace or unsupported inconsistent-comparator behavior is promised. Nested independent scans own independent scratch. There is no new exported symbol, persistent index or lock.

## Fixed experiment and source identity

Baseline complete production SHA256: `feefc376c3f19b9fe9de0266f0a24da2ad179af0360000874235da54cd8562ab`. Candidate: `f19fc1689020d73f57fdc6e6cde0baeb863151dfafa405fac23b491288963a0b`. The actual complete directory section is compiled into the same ordinary benchmark caller, selected through a volatile actual Scandir function pointer. Real host directories supply every inode/type/name/position; an independent real `readdir` enumeration plus stable reference sort supplies the expected result. Every timed operation checks every returned field, frees every entry/result, and each fresh process removes its owned directory before reporting completion.

CPU uses `CLOCK_PROCESS_CPUTIME_ID`; wall uses `CLOCK_MONOTONIC`. The program is single-threaded. Directory construction and independent reference enumeration precede measurement. **First-call means the first complete entry after this setup, not process startup.** The three following measured batches all remain in each process cost: sum CPU/wall intervals divided by sum calls. None is dropped as a warmup.

The written predeclaration was saved at 20:38:50UTC before the first process. The two fixed three-block parts ran 20:39:09.446–20:39:24.588 and 20:39:24.635–20:39:40.375UTC. They preserve global blocks 0–5, rotating case order, baseline/candidate/candidate/baseline order and all planned calls. Root, Java and Media confirmed no concurrent local build, replay, compression or benchmark. This coordination does not establish isolation from all system activity.

Final ordinary builds use GCC 14.2, `-std=gnu++20 -O3 -DNDEBUG -fno-exceptions -fno-builtin -mno-ms-bitfields -fsized-deallocation`, with ordinary allocators and no sanitizer/counter hooks. Those optimization/project options match the inspected configuration; the host GCC binary is not the LLVM Windows product binary. Full commands, generated sources, both actual binaries and source hashes are archived.

## Complete steady results

Each variant/case has 12 fresh process costs. CPU p95 here is the distribution of complete batch-average process costs, not a per-operation latency quantile. Negative changes are reductions. Mean and paired results are printed alongside medians to expose asymmetric tails.

| Case | CPU µs/call, baseline → candidate | Median CPU | Median wall | Mean CPU | CPU p95 µs/call | Paired mean CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 1.257 → 1.272 | +1.24% | +0.20% | -1.14% | 1.792 → 1.717 | -0.94% |
| single | 1.488 → 1.386 | -6.87% | -6.81% | -8.31% | 2.056 → 1.735 | -7.36% |
| tiny2 | 1.451 → 1.611 | +11.05% | +10.97% | +6.70% | 1.740 → 1.821 | +6.81% |
| small16 | 3.780 → 3.864 | +2.23% | +2.32% | +20.74% | 4.255 → 7.138 | +20.85% |
| floor127 | 63.283 → 65.543 | +3.57% | +4.18% | +2.79% | 72.175 → 74.097 | +2.91% |
| floor128 | 66.675 → 23.797 | -64.31% | -64.25% | -66.10% | 96.409 → 30.424 | -65.73% |
| sorted1024 | 171.965 → 166.821 | -2.99% | -3.64% | +5.39% | 206.904 → 235.394 | +5.11% |
| nearby1024 | 173.639 → 177.670 | +2.32% | +2.84% | -1.62% | 213.301 → 208.087 | -1.30% |
| random1024 | 2,031.324 → 212.525 | -89.54% | -89.31% | -89.14% | 2,535.704 → 286.975 | -89.08% |
| reverse1024 | 3,454.213 → 186.712 | -94.59% | -94.53% | -94.80% | 4,650.684 → 222.394 | -94.72% |
| lexical1024 | 4,816.730 → 241.200 | -94.99% | -94.92% | -95.18% | 5,894.563 → 267.700 | -95.15% |
| ties1024 | 1,796.015 → 249.758 | -86.09% | -86.24% | -85.44% | 2,501.306 → 452.306 | -85.58% |
| reverse4096 | 56,124.197 → 972.382 | -98.27% | -98.29% | -97.94% | 61,667.492 → 2,002.343 | -97.96% |
| nosort4096 | 954.164 → 910.687 | -4.56% | -5.58% | -2.89% | 1,257.084 → 1,337.905 | -3.03% |
| filtered4096 | 404.463 → 416.102 | +2.88% | +2.71% | +0.56% | 501.793 → 484.631 | +1.45% |

## All first-call results

P95 uses linear interpolation at(n−1)×0.95 over 12 complete first calls per variant. These sparse descriptive tails do not establish a startup service-level bound. Individual observations and maxima remain in JSON and raw logs.

| Case | First CPU median µs | CPU change | First CPU p95 µs | First wall median µs | Wall change | First wall p95 µs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| empty | 6.832 → 7.150 | +4.65% | 54.338 → 19.035 | 8.062 → 8.467 | +5.03% | 55.981 → 20.555 |
| single | 10.830 → 10.914 | +0.78% | 17.818 → 25.063 | 12.017 → 12.225 | +1.73% | 19.526 → 26.338 |
| tiny2 | 4.756 → 4.745 | -0.23% | 5.745 → 51.237 | 5.942 → 5.982 | +0.66% | 7.026 → 52.593 |
| small16 | 8.535 → 7.780 | -8.84% | 10.666 → 9.862 | 9.860 → 9.040 | -8.32% | 11.934 → 11.253 |
| floor127 | 65.660 → 63.922 | -2.65% | 81.811 → 102.979 | 67.175 → 65.184 | -2.96% | 83.191 → 117.673 |
| floor128 | 64.579 → 28.530 | -55.82% | 788.818 → 41.290 | 65.775 → 29.719 | -54.82% | 790.682 → 42.317 |
| sorted1024 | 336.896 → 483.683 | +43.57% | 777.340 → 1,006.778 | 338.681 → 485.450 | +43.34% | 779.065 → 1,008.715 |
| nearby1024 | 312.291 → 202.073 | -35.29% | 779.953 → 511.408 | 313.867 → 203.423 | -35.19% | 781.704 → 512.901 |
| random1024 | 2,006.811 → 331.418 | -83.49% | 2,375.211 → 580.453 | 2,008.542 → 340.677 | -83.04% | 2,377.256 → 582.099 |
| reverse1024 | 3,438.517 → 267.866 | -92.21% | 3,938.163 → 481.681 | 3,480.539 → 269.203 | -92.27% | 3,939.684 → 492.076 |
| lexical1024 | 4,933.429 → 413.744 | -91.61% | 7,546.221 → 766.936 | 4,934.824 → 415.296 | -91.58% | 7,547.874 → 768.770 |
| ties1024 | 1,819.071 → 292.933 | -83.90% | 5,279.210 → 461.659 | 1,820.592 → 294.513 | -83.82% | 5,280.626 → 463.406 |
| reverse4096 | 56,812.266 → 2,066.729 | -96.36% | 88,028.668 → 2,355.138 | 57,199.759 → 2,120.943 | -96.29% | 88,310.753 → 2,357.316 |
| nosort4096 | 1,277.228 → 1,373.093 | +7.51% | 2,998.212 → 5,768.330 | 1,279.251 → 1,374.891 | +7.48% | 3,000.278 → 5,769.975 |
| filtered4096 | 489.461 → 482.041 | -1.52% | 797.099 → 1,178.357 | 491.152 → 483.813 | -1.49% | 849.917 → 1,180.669 |

## Paired blocks and retained variability

Each block compares the arithmetic mean of its two process costs per variant. The descriptive interval resamples the six fixed block ratios 10,000 times with seed 178; it does not establish a universal platform or small-workload budget.

| Case | Six steady CPU block changes (%) | Descriptive paired mean 95% interval (%) |
| --- | --- | --- |
| empty | -3.95, -6.10, -2.20, +7.40, +3.48, -4.27 | -4.43 .. +3.20 |
| single | -18.13, -6.54, +0.47, +5.45, -2.64, -22.75 | -15.28 .. +0.44 |
| tiny2 | +3.86, +11.96, +4.91, +19.55, +0.07, +0.51 | +1.83 .. +12.67 |
| small16 | +12.08, -1.24, +64.73, -0.93, +31.24, +19.25 | +4.50 .. +40.57 |
| floor127 | +2.15, +14.06, +1.84, +2.07, -3.95, +1.30 | -0.98 .. +7.91 |
| floor128 | -66.01, -61.51, -62.37, -67.05, -69.37, -68.08 | -67.95 .. -63.47 |
| sorted1024 | -0.34, -7.62, -7.99, +24.83, +13.25, +8.52 | -3.84 .. +14.85 |
| nearby1024 | +1.87, -1.42, +10.62, -8.11, -5.70, -5.06 | -5.68 .. +3.94 |
| random1024 | -90.40, -89.52, -89.79, -88.19, -89.55, -87.06 | -89.89 .. -88.11 |
| reverse1024 | -94.67, -95.77, -93.99, -93.67, -95.67, -94.54 | -95.36 .. -94.10 |
| lexical1024 | -95.45, -95.67, -94.71, -94.69, -95.54, -94.86 | -95.46 .. -94.85 |
| ties1024 | -86.60, -86.83, -85.68, -84.55, -82.43, -87.41 | -86.75 .. -84.16 |
| reverse4096 | -97.85, -98.11, -98.52, -98.14, -97.03, -98.08 | -98.27 .. -97.55 |
| nosort4096 | +7.93, -6.00, -6.65, -5.30, +13.01, -21.15 | -11.25 .. +5.83 |
| filtered4096 | +0.31, +5.46, -2.73, +20.21, -12.65, -1.88 | -5.82 .. +10.25 |

## Actual work and scoped storage

The complete-source sanitizer fixture records 62 workload rows per revision, including sorting orders, 127/128/129 boundaries, uneven tails, failed scratch and nested cases. The following representative values are observed callback counts and actual delegated C allocator live-byte counters, not an algorithm-only model.

| Entries/order | Baseline comparisons | Candidate comparisons | Extra scratch bytes | Candidate peak scoped bytes |
| --- | ---: | ---: | ---: | ---: |
|128 reverse|8,128|1,496|1,024|37,888|
|129 reverse|8,256|1,504|1,032|39,200|
|1,024 sorted|1,023|1,023|0|294,912|
|1,024 nearly sorted|1,534|1,534|0|294,912|
|1,024 random|261,555|16,903|8,192|303,104|
|1,024 reverse|523,776|13,440|8,192|303,104|
|1,024 tied keys|225,789|16,480|8,192|303,104|

All 124 rows are retained. Every successful optional allocation is exactly 8N bytes, every completed sort retains the original live output allocation total, and every complete caller frees it to zero. Denied scratch makes exactly one attempt, preserves comparator errno and finishes through the original sort. These counters exclude unrelated C++ runtime/filesystem allocations and are not whole-process RSS. Static Windows `.su` receipts show `Scandir`/`Scandirat` wrapper frames unchanged at 72 B x64/64 B ARM64; the inner frame increases quoted above are static compiler data, not measured peak stack usage.

## Preserved pilot

The original 60-process / 240-observation exploratory campaign ran 19:56:47.510–19:56:49.630UTC. It used GCC 14.2 O2 and shorter batches. The production source was already identical, but its caller/optimization/repetition settings differ from the final project-shaped O3 campaign. Both original generated sources and binaries, all logs and the original summary remain in the archive. The following independently recomputed pilot values are historical; the final campaign does not replace or erase them.

| Case | O2 pilot steady CPU change | O2 pilot first CPU change |
| --- | ---: | ---: |
| empty | +3.35% | +4.21% |
| single | +24.40% | +158.26% |
| tiny2 | +72.64% | +2.84% |
| small16 | -11.93% | +14.95% |
| floor127 | -9.87% | -0.06% |
| floor128 | -61.56% | -61.80% |
| sorted1024 | -6.15% | +165.98% |
| nearby1024 | -13.64% | +11.23% |
| random1024 | -76.58% | -86.00% |
| reverse1024 | -94.96% | -91.35% |
| lexical1024 | -95.43% | -91.65% |
| ties1024 | -85.25% | -85.37% |
| reverse4096 | -98.36% | -96.32% |
| nosort4096 | -6.41% | -13.10% |
| filtered4096 | -11.39% | -7.26% |

In particular the pilot tiny2 +72.64%, single +24.40% and sorted first +165.98% remain visible. No equality of O2 and O3 timing distributions is asserted.

## Correctness, source qualification and platform limits

The current production fixture passes 198,874 checks under ASan/UBSan; the unchanged baseline passes 2,184,791 checks with only new efficiency budgets disabled. The differing totals include callback-by-callback assertions; they are not distinct API cases. Both use the real complete directory implementation and real host filesystem. All existing directory contracts and original allocation/reader/closedir failure controls remain. Nine separately compiled mutations are rejected at their designated stable-order, copy-back, scratch lifetime/size/retry, errno, OOM completion, work-budget or tiny-path assertion. Independent Media replay passes the same 198,874 checks and all nine mutations.

The guest C corpus exercises all four public scan symbols at 127/128/129/257 entries and stable ties, errno and owned parent-fd behavior. Its new include passes C syntax checking. Complete baseline and candidate `posix_bionic.cc` TUs compile with strict shipping-shaped LLVM-MinGW flags for Windows x64 andARM64; this is compilation evidence. **Actual Windows enumeration, guest ABI execution, full app/ELF/runtime acceptance and an Android supplier execution remain pending.** No hardware/whole-application result is inferred from these host probes.

Because this file is part of the existing OFD qualification, the new independently pinned single-file transition strictly reverses only Scandir back to published #413 before the unchanged Memrchr and OFD/shared-stdio normalization. Exact reverse and forward endpoints are checked. All six prior manifests/patches,71 original source digests and original approval predicates remain unchanged. The old Memrchr negative controls run on the strictly reconstructed predecessor;26 new controls exercise the full Scandir chain, including repinned unrelated edits. The retained Memrchr report validator first reconstructs its exact predecessor and also checks that its measured Memrchr body is unchanged; all original numeric/archive checks remain.

Current composed gates pass26 original policy controls +4 property repins +23 shared-stdio +20 Memrchr +26 Scandir, supplier36+1, the maintained vector policy and the unchanged Memrchr828-member report with 10,830 checks. An initial invocation used a nonexistent vector helper filename; that command/error remains archived, and the actual maintained `tools/net-tcp-priority/test-vector-policy.py` passes. No source/gate change was made to turn that invocation error into a pass.

## Canonical current-main composition

The measured local baseline `ccd41b07…` is the exact automatic merge of public #413 `7e205878…` and main `d3076fc4…`. Main subsequently merged #297 as `910bebe1…`. Its 15 paths affect epoll owner implementation/fixtures; they do not touch the measured complete posix source, directory fixtures, workflow recipe or qualification ledger.

The final local prerequisite is an independently reconstructible automatic merge with ordered parents `[7e205878cec54dbbd52bbac8a90b22e190798a5e, 910bebe1edaf105e289edd60841b8354fd847bea]`, tree `ed45f6b00f03218cb2e6a4a463656d3840118f0f`, local commit `0e440b8df14ede52dfc3bb6a99dec07fb503fb76`. All measured source/fixture bytes are preserved; complete baseline source SHA remains identical. No manual resolution, retiming or claim about newly integrated epoll runtime was introduced. The subsequent Scandir leaf adds its unconditional host and evidence gates while retaining every inherited command.

The fresh 20:46:40 UTC file census covers 86 PRs and is archived: only dependency #413 and existing unrelated FIFO #256 touch the production file. This is overlap evidence, not an approval of those other PRs. Historical local IDs remain labeled as local; final publication separately verifies the remote trees and ordered ancestry.

## Evidence and reproduction

Archive: `docs/research/data/scandir-stable-sort-2026-11-23.tar.gz` — SHA256 `cae0558ee56a97d3bee5ff5b2e5a9e816d3fd53707ae30ddab541502c406f770`, 1,342,719 bytes, 728 regular members. Its manifest hashes every member except itself. It includes420 fresh timing processes / 1,680 observations, all ordinary binaries and generated sources, full raw logs, resource/failure controls, strict Windows commands/static-frame receipts, both baseline composition proofs and independent source/runtime review. JSON contains every numeric distribution, paired block and qualification limit.

```sh
python3 tools/test_scandir_report.py
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 tools/test_scandir_sort.py --sanitize --negative-controls --output-dir /tmp/scandir-current
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 tools/test_scandir_sort.py --sanitize --baseline --source-ref 7e205878cec54dbbd52bbac8a90b22e190798a5e --output-dir /tmp/scandir-baseline
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-selected-supplier.py
python3 tools/net-tcp-priority/test-vector-policy.py
python3 tools/test_memrchr_report.py
```

Local LeakSanitizer alone is disabled because this executor uses ptrace; ASan/UBSan are active and the required CI invocation remains unchanged. `--build-benchmark` builds the complete ordinary caller; the archived predeclaration/driver records the fixed final schedule. Reproduction is a new observation, not a reason to discard the published cold/tiny/tail costs.

## Supplier-bundle CI integration after publication

Published Scandir #430 at `0654cde8fb473b463a0ac5ad29149d6a5b2037c0` was composed with main `52513f0fc3f5fc657138cf01429cf969b2f21649`, which merged #409 after the recorded authenticated quota failures. The local automatic merge is `1e3241af1ab734c4c08da29c6460dc8a07119a1a`, with ordered parents `[0654cde8fb473b463a0ac5ad29149d6a5b2037c0, 52513f0fc3f5fc657138cf01429cf969b2f21649]` and tree `e821b52bbaf8f8582ec96313ad1573a1d08ee755`. The only shared path is the automatically merged CI workflow. All fourteen incoming paths are retained, and the full workflow equals main's bytes plus the four existing Memrchr/Scandir commands. The public #413 ancestry, every measured production/fixture input, all previous qualification files, and the original 728-member evidence archive remain unchanged.

The incoming supplier job authenticates the original committed batch once, then publishes the verified files with a manifest-bound ledger. Each consumer verifies the exact successful bundle producer, archive identity, run/source/checkout, every file, and live producer/artifact identity again after download. An earlier ledger attempt may be retained within the same run; the current copied-job predicate remains exact. The existing single-artifact and retained-installer functions are byte-identical. The published-reset wait budget and corresponding job timeouts increase together; the bounded request counts and source/digest/producer predicates remain intact.

The original Scandir report verifier correctly rejected the changed whole-workflow hash. That unmodified failure is retained as a negative receipt. `tools/scandir_ci_integration.py` now accepts only the exact reviewed composed workflow and the complete old verifier with two fixed source edits. It pins the entire historical source map and archive identity, requires the complete incoming workflow after removal of the four retained feature commands, and reconstructs the original verifier byte-for-byte. Only those two sources are presented to the old verifier as their archived predecessors; every other current source still reaches its original hash assertion. All earlier JSON fields, statistics, source pins and archive bytes are preserved in place, with the integration recorded in one new section.

On this exact composition, the maintained report passes all 9,494 historical integrity/arithmetic checks and thirty new endpoint, omitted-input and repinning controls. The actual supplier-bundle suite passes 16 tests, the batch suite passes 34, and the quota suite passes 18. The original selected/source chain, 36+1 supplier controls, vector policy, and unchanged Memrchr report's 10,830 checks also pass. A mistyped invocation of a nonexistent supplier helper is retained separately; invoking the maintained `test-selected-supplier.py` succeeds without any source or gate change.

The separate integration receipt archive is `docs/research/data/scandir-supplier-ci-2026-11-23.tar.gz`; its exact digest, member manifest, canonical proof, source snapshots and complete gate logs are recorded in the JSON integration section. The original archive above remains byte-identical. The new proof script accepts an explicit repository path and reconstructs the ordered public-parent merge, so the local commit identifier is not required for reproduction from the subsequently published history.

This integration adds no timing samples or runtime changes. All tiny-directory, small-directory, first-call, tail and stack/storage costs reported above remain applicable to the measured inputs. Actual authenticated bundle transport, Windows/guest execution and the new full-CI result remain separate pending evidence.
