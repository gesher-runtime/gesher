# Calling-thread pthread key reads without the registry mutex

This is a bounded **#56 T12** efficiency change to `pthread_getspecific`. It removes the process-wide key-registry mutex from a read of the calling thread's own value. Key creation, deletion, assignment and destructor selection keep their existing serialization. It does not complete dynamic ELF TLS, thread lifecycle, or the broader native roadmap.

The final Linux source-component measurement includes real thread creation, assignment, every checked read, native pthread TLS destruction, joins and key deletion. One million reads on one thread use **90.19% less process CPU**; the measured eight- and 32-reader cases, including concurrent independent-key churn, use **98.06–98.97% less process CPU**. These numbers describe the measured component, not application startup or whole-runtime performance. The fixed global key arrays grow by **896 bytes**. The fresh one-read control costs **40.9 µs more CPU and 65.1 µs more wall time**, and remains an unresolved cold-start budget tradeoff.

The earlier process-clock-only campaign produced suspect short-lived-worker CPU receipts. Its entire 1,296-observation campaign, adverse controls, binaries' hashes, original C++ driver and investigation are preserved; its CPU reductions are **not** accepted as performance evidence. The final campaign records an independent CPU lower bound for every worker and rejects any process receipt that does not cover those workers plus its calling thread.

## Scope and ownership

The previous `gesher_bionic_pthread_getspecific` took `g_keys_mutex` for every read, even though the value and its assigned generation live in the calling thread's arrays. Independent reader addresses therefore shared one mutex. The new global live-generation table publishes a complete generation in one atomic observation. Zero means unused; a live value must equal the calling thread's recorded generation before its value can be returned.

The slot's existing monotonically increasing generation still advances under the registry mutex. Creation publishes that generation with release ordering after initializing the destructor. Deletion publishes zero with release ordering. A read uses acquire ordering and touches no unprotected global generation or destructor state. A synchronized deletion or replacement must be observed by a later read; a read that overlaps a mutation can linearize at its own generation observation. An old calling-thread value cannot match a reused slot's new generation. A slot at `UINT64_MAX` remains retired after deletion rather than wrapping and recreating an old identity.

Assignment and destructor selection continue to hold the original mutex. The destructor is still selected under that lock and invoked after it is released, preserving reentrant delete/recreate and the existing four destructor rounds. A callback already selected before concurrent deletion retains the existing behavior. The atomic observation does not prolong a caller-owned value's lifetime, keep a destructor's module loaded, or make an otherwise unsafe application lifetime valid.

The Android design reference is the generation/sequence handling in [Android 16 r4 `pthread_key.cpp`, pinned at `f22516cb`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/pthread_key.cpp). The archived source hash is `18adb3106579c24140534295936f739fb09575247e70612284968f12daa18567`. This is a source reference, not an executed AOSP differential oracle; Gesher's existing key encoding and public return behavior remain in place.

## Measured boundary and method

The current base is `98215dd7e798090fb5ab6160490ae78f1793ab3a`. The measured baseline was built from `fdf4861b608d7a5fe8c0fa6c8a285dfbdf573d91`; all baseline production and included ABI/cleanup header bytes are identical at these two commits. The candidate production hash is `ce833eb1e951fb10ed42235e73c66a074acf57586572188b8dd7d1da5eb8a223`; the baseline hash is `b8b14e916e7380261bc2aa9241da9690b95c0f67883f868d117d7fa54f364817`.

`tools/measure_bionic_key_lookup.py` extracts the complete actual key state, key API, explicit-exit and host TLS-exit sections into their own translation unit. The driver is a separate translation unit, with no LTO, `-O2`, strict warnings, the actual guest calling convention and no builtin substitution. It supplies the Windows `ThreadRecord` boundary. It executes actual host pthread TLS cleanup; it does not execute Windows thread registration, ART, guest ELF loading or the entire shim DLL. Both variants use the same driver and ordinary allocation behavior.

Each timed component starts before key creation and ends after all workers have returned through real TLS destructors, joined, and their keys have been deleted. Reader loops check every actual `pthread_getspecific` result and preserve an errno sentinel; each worker must execute exactly one matching destructor. The concurrent-churn writer creates and deletes 4,000 independent keys. The mutation control performs 40,000 complete create/set/set-null/delete cycles.

The accepted campaign ran **2026-11-23 13:43:51.991398–13:44:35.466905 UTC**, with all other three lanes explicitly idle. There are four ABBA rounds for each of nine workloads, with a **fresh process per workload invocation**: 144 timed processes and 1,296 observations. Each steady process records ten observations; samples zero and one are retained as warm-ups. The table uses the median of each process's eight remaining observations, then the median of the eight process medians per variant. The one-read cold case runs only once in each fresh process, giving eight observations per variant. Those cold values are descriptive and do not establish startup equivalence or a release budget.

The clock check is part of both drivers. Each reader records `CLOCK_THREAD_CPUTIME_ID` from its body entry through its actual TLS destructor; the churn writer records entry through return. The caller's thread CPU and `CLOCK_PROCESS_CPUTIME_ID` interval bracket the complete component. Every receipt must satisfy `process CPU >= caller CPU + sum(worker CPU)`. This sum is a lower bound: it excludes some worker startup/teardown and is not relabeled complete kernel thread cost. All **175,302 individual worker receipts** across the timed and counted runs satisfy this check. The driver stores them individually, not just as a sum. Timing still includes this common clock bookkeeping; it is not an unobserved application timing result.

Separate counted executables add a counter only at actual `g_keys_mutex` acquisition sites. Their clock values are not performance evidence. The timed production translation units have no registry-counter increments. Counter results cover the same complete lifecycle and demonstrate that key reads, rather than all key operations, are removed from the registry lock.

## Complete-component results

CPU and wall values below are milliseconds per complete component. All first calls, warm-ups and adverse values remain in the archive.

| Workload | Baseline CPU | Candidate CPU | CPU change | Baseline wall | Candidate wall | Wall change |
|---|---:|---:|---:|---:|---:|---:|
| One fresh thread, one read | 0.212515 | 0.253416 | **+19.25%** | 0.215979 | 0.281064 | **+30.14%** |
| One thread, 1,000,000 reads | 19.550040 | 1.917604 | −90.19% | 19.588095 | 1.932087 | −90.14% |
| Eight threads, one shared key, 1,000,000 total reads | 312.349789 | 3.216905 | −98.97% | 72.729932 | 1.267231 | −98.26% |
| Eight threads, distinct keys, 1,000,000 total reads | 304.683671 | 3.187924 | −98.95% | 69.065989 | 1.296335 | −98.12% |
| 32 threads, distinct keys, 1,000,000 total reads | 434.937102 | 7.734044 | −98.22% | 71.753483 | 3.316769 | −95.38% |
| Eight readers plus 4,000 create/delete cycles | 321.648899 | 6.238444 | −98.06% | 73.580086 | 1.808605 | −97.54% |
| 64 groups × eight threads × one read | 49.935673 | 44.690878 | −10.50% | 38.857613 | 34.235804 | −11.89% |
| Same 512 thread lifecycles, zero reads | 40.025176 | 39.598310 | −1.07% | 30.127830 | 30.523246 | **+1.31%** |
| 40,000 create/set/set-null/delete cycles | 1.511883 | 1.473536 | −2.54% | 1.511900 | 1.473577 | −2.53% |

The repeated-read improvement is material within this component. The zero-read and mutation paths are controls, not claimed optimizations. The cold one-read increase is retained without a speculative explanation or a claim that it meets a 5% startup budget. No-read wall time is also higher. Eight independent cold observations per variant are insufficient for a confident application tail-latency conclusion.

| Workload | Actual reads / destructor calls | Baseline registry acquisitions | Candidate registry acquisitions |
|---|---:|---:|---:|
| Cold one-read | 1 / 1 | 5 | 4 |
| One reader | 1,000,000 / 1 | 1,000,004 | 4 |
| Eight readers, shared key | 1,000,000 / 8 | 1,000,018 | 18 |
| Eight readers, distinct keys | 1,000,000 / 8 | 1,000,032 | 32 |
| 32 readers, distinct keys | 1,000,000 / 32 | 1,000,128 | 128 |
| Eight readers plus churn | 1,000,000 / 8 | 1,008,018 | 8,018 |
| 512 one-read lifecycles | 512 / 512 | 2,560 | 2,048 |
| 512 zero-read lifecycles | 0 / 512 | 2,048 | 2,048 |
| Mutation control | 0 / 0 | 160,000 | 160,000 |

The three existing global key arrays occupy 2,176 bytes before and 3,072 bytes after: **+896 fixed bytes** replaces 128 booleans with 128 atomic 64-bit live generations. The two calling-thread arrays remain 2,048 bytes. These are exact array `sizeof` values, not process RSS, allocator commitment, the full TLS footprint, or an application memory reduction. No allocation-count or whole-process RAM improvement is claimed.

## Rejected timing and clock investigation

The first maintained campaign ran all nine workloads sequentially inside each of 16 fresh ABBA processes and saved 1,296 timed observations plus 18 counted observations. Its one-reader candidate reported a process-CPU median of **0.317566 ms** for one million out-of-line reads, while wall time was **1.836287 ms**. Inspection of its emitted assembly confirmed the intended process clock and an actual function call on every loop iteration. The discrepancy warranted investigation; it did not establish that a particular kernel, scheduler or clock-resolution mechanism was at fault.

The first campaign also had adverse controls, which remain visible:

| Original process-only campaign control | CPU change | Wall change |
|---|---:|---:|
| 512 one-read lifecycles | +25.35% | +20.53% |
| 512 zero-read lifecycles | +20.10% | +25.21% |
| Mutation control | +12.19% | +12.36% |

A diagnostic variant measured each worker through its actual TLS destructor; one candidate observation then reported 2.699 ms process CPU versus 2.620 ms worker CPU and 2.723 ms wall time. A separate longer diagnostic executed 64 complete create/thread/read/destruct/join/delete lifecycles, with 64 million verified reads and 64 real exits. Its receipts and source are retained. Extending duration alone did not provide an independent accounting bound and was not accepted as the resolution. These diagnostic runs were not controlled performance campaigns.

The final method retains common per-thread clock receipts and enforces their lower bound in every process. It also isolates workloads in fresh processes, so tiny and mutation controls no longer inherit different preceding lock-contention histories from the two variants. No production code was tuned between the two maintained campaigns. The final data do not erase the original regressions or prove their precise cause. A non-quiet 36-process gate pilot verified all clock/value/destructor assertions before the final run; it is archived separately and is not used for the table.

## Correctness and negative controls

The actual-source ASan/UBSan suite passes seven cases: public key behavior, callback mutation/reentrancy, cleanup/C++ TLS/key ordering, real host TLS exit, generation exhaustion, concurrent readers, and progress while the actual registry mutex is held. It covers invalid/deleted/reused keys, independent calling-thread values and errno, 1/8/32 readers on shared and distinct keys, unrelated concurrent mutations, 64 three-phase reuse epochs with eight live readers, obsolete-destructor exclusion, and the existing four destructor rounds. The held-lock test observes completion before releasing the registry lock and before joining the reader, separating get progress from the still-locked exit path.

Six compiled source mutations fail at their named assertions: restoring the global read lock, accepting a stale generation, retaining a deleted publication, publishing the wrong generation, allowing generation wrap, and routing an obsolete value to a replacement destructor. The unchanged baseline passes the six behavioral cases; its held-lock observation correctly returns zero. An independent reviewer replayed both variants and all six mutants against the exact current production/fixture hashes.

The existing Windows exported-adapter executable retains the original ART-before-adapter registration test and gains real `CreateThread` readers for 1/8/32 shared/distinct-key cases. They check own values, a synchronized delete/recreate epoch, empty replacement values, and exactly one matching native-exit destructor. The Windows track requires the new success marker as well as every original marker. The complete production TU and complete exported fixture TU compile with pinned LLVM-MinGW, `-Werror`, for both x64 and ARM64. This is compilation evidence; no local Windows runtime, full DLL link, ART execution or NDK guest-ELF execution is claimed.

LeakSanitizer is unavailable in this host environment, so local ASan/UBSan invocations use `ASAN_OPTIONS=detect_leaks=0`. Remote CI settings and gates remain unchanged. The CI change adds the six negative controls to the existing required source-component invocation.

## Current-main integration and evidence

The unpublished feature was rebased onto `98215dd7`. All eight production/fixture/measurement files are byte-identical to the measured and independently replayed feature. The other two feature paths are CI and the Windows track, which retain incoming main changes plus only their original key additions. The incoming shared-OFD qualification ledger, selected-policy code and framework workflow are exact main bytes. Current main's `a31f1cd…` reusable-workflow pin is an ancestor and has the same `af5b5cc5…` recipe as main. No source-approval receipt is refreshed or weakened for this leaf.

The source-policy selftest, original digest gates and shared-stdio controls pass on this integrated tree. Both complete TUs were recompiled for x64/ARM64 with current-main headers. Baseline production/header byte-identity, all eight unchanged feature hashes, full strict target commands, sanitizer logs and independent review receipts are in the evidence archive. Integration compilation does not imply current-main runtime or performance equivalence outside the measured bytes.

The adjacent [JSON report](pthread-key-generation-2026-11-23.json) gives exact source/fixture/compiler hashes, all process medians, clock-check totals, counter rows, original adverse controls and validation receipts. The [complete evidence archive](data/pthread-key-generation-2026-11-23.tar.gz) contains **351 members**, including all final/original/pilot raw samples and stdout, generated actual production TUs, current compiled source inputs, diagnostic sources, the hash-verified original C++ measurement driver, all six mutant sources/logs and both target-compile receipts. Its manifest hashes every other member. Executables are represented by their hashes and rebuild commands, not bundled binaries. The original Python orchestration source is not relabeled as the final helper; its hash and actual original commands/order remain in the original receipt.

Reproduce the maintained functional and performance component from this PR:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tools/test_bionic_key_contracts.py --sanitize --negative-controls
python3 tools/measure_bionic_key_lookup.py --build-only \
  --source-ref 98215dd7e798090fb5ab6160490ae78f1793ab3a --output-dir /tmp/key-before
python3 tools/measure_bionic_key_lookup.py --build-only --output-dir /tmp/key-after
python3 tools/measure_bionic_key_lookup.py \
  --baseline /tmp/key-before --candidate /tmp/key-after \
  --rounds 4 --samples 10 --output /tmp/key-abba.json
```

Run the final command with unrelated compilations and benchmarks idle. It retains every observation and rejects changed executable hashes, failed value/destructor assertions, wrong workload receipts, counter/timing mixing, or process CPU below the independently measured thread subtotal. Full fork CI, real Windows native/ART execution and application-level performance remain required before release acceptance.
