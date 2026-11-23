# Calling-thread pthread key assignment without the registry mutex

This is a bounded **#56 T12** efficiency leaf for `pthread_setspecific`, dependent on the live-generation publication already proposed in **#384**. It removes the global key-registry acquisition from assignment of the calling thread's own value. Creation, deletion, native-exit arming, destructor selection and the existing four destructor rounds retain their ownership contracts. This does not complete dynamic ELF TLS, library unloading or the wider native roadmap.

The measured Linux source component includes key creation, actual thread start, every checked assignment/read, real host TLS destruction, joins and key deletion. For 786,432 assignments and reads, process CPU falls **77.00% with one writer** and **95.56–98.33% with 2/8/32 writers**. Repeated clear and concurrent independent-key churn also improve substantially. The measured single-thread component falls from **16.149 ms to 3.715 ms**; the eight-writer shared-key component falls from **285.961 ms to 4.786 ms**. These are complete source-component results, not application or Windows execution claims.

The cold one-assignment component costs **42.18 µs more process CPU (+18.22%) and 43.11 µs more wall time (+19.34%)** in the final campaign. The first tiny-lifecycle sample also costs more CPU, and the first read-only control has higher wall time. Those observations and the pilot's adverse read-only control remain visible. No universal 5% or cold-start release acceptance is claimed.

## Baseline and the exact change

The measured baseline is the canonical merge of published #384 head `92c64a1dfc8b0c1a78fcadb234b3b6e90a1c8d8e` with public main `e505ad227523aaca566d05b8af6e466495c819cc`. Its complete tree is `b56335c8f947c05cea71ae98df3afde79fb8561c`, with those ordered parents and no manual source resolution. The measurement receipts name the original local commit `5f3eb58710b71dbf2f6d039ce8bcec4afdb19a42`. Public baseline **`8b3f0bc299ec16141012b1ae2bc2a07358d078cf` has the identical tree and ordered parents**. Both identities and the independent canonical composition proof are archived; reproduce with the public object.

Baseline `pthread_bionic.cc` SHA-256 is `ce833eb1e951fb10ed42235e73c66a074acf57586572188b8dd7d1da5eb8a223`; the candidate is `f40ce16ef94a1315093fc2285bb6f4e1ac82eb969a10733797436ce36668dd3d`. The getter is unchanged from #384. This comparison measures the additional setter change rather than adding the earlier getter improvement to its result. The earlier key-generation report remains historical evidence for that separate PR.

Previously, each assignment locked `g_keys_mutex`, checked the live slot, then copied the global generation into the calling thread's generation slot. The candidate performs one acquire load of the same atomic live generation already used by the getter, rejects zero, and stores that exact observed generation beside its calling-thread value. It neither reads the unprotected global generation/destructor arrays nor introduces another cache or publication table.

A synchronized deletion or replacement must be reflected by a subsequent live-generation observation. A setter overlapping deletion/reuse may linearize at its earlier observation; its local value remains stamped with that generation. A later getter or destructor pass rejects it after the slot has changed. Reloading the generation between the value and generation stores would wrongly adopt an obsolete value into a replacement key, so the deterministic scheduling control specifically rejects that mutation. This snapshot does not extend a caller-owned value's lifetime or keep a destructor's code loaded.

`ArmHostExit` still runs only after a successful non-null assignment. Null/invalid assignments, first native-thread arming, managed-thread handling, destructor rearming and reentrant create/delete behavior retain their prior paths. Creation/deletion/destructor selection still use the original mutex, and `UINT64_MAX` exhaustion still prevents generation reuse. A destructor selected before overlapping deletion retains the existing behavior.

The design reference is the sequence/generation approach in [Android 16 r4 `pthread_key.cpp`, pinned at f22516cb](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/pthread_key.cpp), source SHA-256 `18adb3106579c24140534295936f739fb09575247e70612284968f12daa18567`. Its source and commit receipt are archived. This is a source reference, not an executed Android differential oracle; Gesher's key representation and public error behavior remain in place.

## Complete measurement boundary

The maintained helper extracts the complete actual key state/API and explicit/native TLS-exit sections into a separate translation unit. A separate C++ driver supplies the Windows `ThreadRecord` boundary and calls the actual guest-convention functions. It executes real host pthread TLS callbacks, including `ArmHostExit`. Both builds use GCC 14.2, `-O2`, strict warnings, ordinary allocation, `-fno-builtin` and no LTO. It does not execute Windows registration, guest ELF loading, ART or the complete shim DLL.

Each timed component begins before key creation and ends after its threads have returned through their actual TLS destructors, joined and had their keys deleted. Every assignment/read value is checked, only the final retained value may reach its destructor, and the errno sentinel must survive. The clear workload performs 262,160 assignments including initial/final values, 262,144 verified reads and eight real destructors. The churn workload adds 4,000 complete independent create/delete cycles. Tiny and zero-assignment controls each include 256 real thread lifecycles. The read-only control has one setup assignment and 786,432 unchanged getter calls.

All source and ordinary binaries were frozen before the pilot and remain identical in the final campaign. Final timing used six ABBA rounds per workload, split into two predeclared three-round windows while the other three lanes were explicitly idle:

- **17:26:59.199632–17:27:25.205288 UTC**, 144 fresh processes.
- **17:35:39.051812–17:36:05.606006 UTC**, another 144 fresh processes.

The 288 timed processes produced **2,400 observations**. Every non-cold process ran nine complete components; samples zero and one remain preserved as warm-ups. The table uses each process's median of its seven steady observations, then the median of the twelve independent process medians per variant. The one-assignment cold case runs once per fresh process, yielding twelve first calls per variant. Its row below is therefore a first-call result, not warmed steady state. The JSON retains every process median, first result and six paired ABBA rounds; its descriptive bootstrap intervals are not application/scheduler generalization.

Every component records caller-thread and process CPU. Assigned workers record thread CPU from body entry through their real TLS destructor; no-assignment/churn workers record entry through return. All **127,848 individual worker receipts** across final timed and counted runs are retained. Each process receipt must cover the caller CPU plus the sum of worker CPU. This subtotal excludes parts of thread startup/teardown and is a lower bound, not relabeled whole kernel-thread cost. The same clock bookkeeping is present in both variants. Separate counted binaries observe actual mutex-acquisition sites; their timings are not used for performance results.

## Final complete-component results

| Complete component | Baseline CPU ms | Candidate CPU ms | CPU change | Baseline wall ms | Candidate wall ms | Wall change |
|---|---:|---:|---:|---:|---:|---:|
| One fresh thread, one assignment/read/exit | 0.231462 | 0.273637 | +18.22% | 0.222913 | 0.266025 | +19.34% |
| One thread, 786,432 assignments and reads | 16.149157 | 3.714727 | −77.00% | 16.359736 | 3.736363 | −77.16% |
| Two threads, one shared key, 786,432 assignments/reads | 86.935784 | 3.859532 | −95.56% | 45.760174 | 2.139813 | −95.32% |
| Eight threads, one shared key, 786,432 assignments/reads | 285.961438 | 4.785740 | −98.33% | 62.673254 | 1.605684 | −97.44% |
| Eight threads, distinct keys, 786,432 assignments/reads | 277.096723 | 4.821162 | −98.26% | 62.258260 | 1.793473 | −97.12% |
| 32 threads, distinct keys, 786,432 assignments/reads | 391.376204 | 9.801172 | −97.50% | 61.783950 | 3.949547 | −93.61% |
| Eight threads, repeated clear/read, final real exit | 88.997051 | 1.902079 | −97.86% | 20.634427 | 0.732842 | −96.45% |
| Eight writers plus 4,000 independent create/delete cycles | 291.458424 | 10.035076 | −96.56% | 64.198969 | 2.583979 | −95.98% |
| 32 groups × eight threads × one assignment/read | 23.271276 | 23.029600 | −1.04% | 19.167013 | 18.260797 | −4.73% |
| Same 256 thread lifecycles, zero assignments | 19.790044 | 19.928621 | +0.70% | 15.551797 | 15.426947 | −0.80% |
| 40,000 create/delete cycles, zero assignments | 0.740923 | 0.701106 | −5.37% | 0.740854 | 0.701083 | −5.37% |
| One setup assignment and 786,432 reads | 1.847292 | 1.854736 | +0.40% | 1.860451 | 1.869625 | +0.49% |

The repeated-assignment CPU and registry-work reductions are material within this boundary. Zero-assignment, create/delete and read-only paths are controls, not claimed optimization targets. In the final steady observations their CPU/wall differences are small except the create/delete control's lower time, which is not attributed to a changed implementation. The cold and first-call penalties remain explicit:

| First component in each fresh process | Baseline CPU ms | Candidate CPU ms | CPU change | Wall change |
|---|---:|---:|---:|---:|
| One fresh thread, one assignment/read/exit | 0.231462 | 0.273637 | +18.22% | +19.34% |
| 32 groups × eight threads × one assignment/read | 20.657682 | 22.317386 | +8.03% | +7.26% |
| Same 256 thread lifecycles, zero assignments | 18.954983 | 19.773273 | +4.32% | −1.05% |
| 40,000 create/delete cycles, zero assignments | 0.704807 | 0.733498 | +4.07% | +4.06% |
| One setup assignment and 786,432 reads | 1.994446 | 2.085768 | +4.58% | +5.63% |

The cold CPU difference is +42.18 µs, the first tiny-lifecycle CPU difference is +1.660 ms across its 256 threads, and first read-only wall rises 5.63%. Small-sample paired summaries vary and can differ from the ratio of marginal medians; all pairs and descriptive intervals are retained. They do not establish either a universal regression or startup equivalence. A meaningful release budget for these first-use costs remains unresolved. No threshold, workload or source was tuned after observing the final measurements.

## Actual registry work and memory

The counter lives only in separately compiled measurement code at actual registry lock sites. Complete-lifecycle counts preserve creation, deletion, churn and destructor selection:

| Workload | Assignments / reads / destructors | Baseline registry acquisitions | Candidate registry acquisitions |
|---|---:|---:|---:|
| cold-single | 1 / 1 / 1 | 4 | 3 |
| single | 786,432 / 786,432 / 1 | 786,435 | 3 |
| shared2 | 786,432 / 786,432 / 2 | 786,436 | 4 |
| shared8 | 786,432 / 786,432 / 8 | 786,442 | 10 |
| distinct8 | 786,432 / 786,432 / 8 | 786,456 | 24 |
| distinct32 | 786,432 / 786,432 / 32 | 786,528 | 96 |
| clear8 | 262,160 / 262,144 / 8 | 262,170 | 10 |
| churn8 | 786,432 / 786,432 / 8 | 794,442 | 8,010 |
| tiny-lifecycle | 256 / 256 / 256 | 1,024 | 768 |
| no-set-lifecycle | 0 / 0 / 0 | 512 | 512 |
| create-delete-control | 0 / 0 / 0 | 80,000 | 80,000 |
| read-only-control | 1 / 786,432 / 1 | 4 | 3 |

The exact count formula is `2 * created_keys + destructor_calls`, plus assignment calls in the locked-setter baseline. Every one of the **48 final counted observations** satisfies it. This demonstrates removal of per-assignment lock work, not removal of all synchronization or an assertion that exit never waits.

There is **zero incremental static or calling-thread array storage** from this setter change. Both variants have 3,072 bytes of global key arrays and 2,048 bytes of calling-thread value/generation arrays. The prerequisite #384 still carries its separately documented **+896 fixed bytes versus main**. These are array `sizeof` values, not full TLS footprint, RSS, committed memory, allocation counts or an application memory-saving claim.

## Original pilot and adverse controls

The original two-round pilot is retained in full: **96 fresh processes, 272 timed observations and 24 counted observations**, using the same binaries. It showed substantial repeated-assignment reductions but a read-only control at **+8.10% CPU/+8.02% wall** and a first zero-assignment component at **+13.72% CPU**. Its cold row happened to improve 16.93%; that result is not substituted for the adverse final cold row.

The final six-round campaign was declared before it ran, with more complete observations per process and separate first-call reporting. It is not a series of selected favorable reruns. All pilot/final records, warm-ups and controls remain. Inspection also found identical getter instructions and entry/live-array addresses in the two ordinary executables; those disassemblies are archived. That observation does not explain the complete read-only lifecycle variation or establish its cause. No production or compiler tuning was performed between the pilot and final campaigns.

## Contracts, negative controls and remaining runtime gates

The actual-source ASan/UBSan entry passes **413 positive assertions** across ten ordinary cases and one separately compiled scheduling-barrier case. It retains the prior public, invalid/deleted/reused key, callback mutation, cleanup/C++ TLS/key ordering, native exit, exhaustion and getter progress checks. New cases cover 1/2/8/32 writers, shared/distinct keys, overwrites/clears, independent thread values and final destructor ownership; first native-exit arming, null/invalid and managed-thread behavior; and assignment completion while the real registry lock remains held, before the still-serialized exit path.

The separate scheduling fixture inserts a barrier immediately after the actual setter generation load in generated test source. There is no hook in ordinary production or timed code. Two paused writers allow synchronized deletion/reuse before their local stores: one then tests getter rejection/fresh assignment, and the other exits directly with the stale assignment. Both old-value adoption and obsolete-destructor delivery are rejected.

Twelve separately compiled mutations fail their designated assertions: the six retained reader/generation/destructor controls, plus restoring the global setter lock, reloading the generation after the observed snapshot, omitting the generation stamp, skipping null clears, omitting native-exit arming and arming on an invalid/null path. The inherited wrong-publication mutant now targets the existing destructor assertion because getter and setter share the wrong live publication; the old expected get/set message no longer isolates it. The initially mis-targeted run is preserved, no original assertion is deleted, and the final control fails at the intended real ownership assertion. An initial unused-callback fixture compilation failure is also retained with its corrected test-only guard.

The unchanged setter baseline passes the behavioral cases and observes zero progress while the registry mutex remains held. An independent reviewer replayed the exact candidate, all twelve mutants and the post-load barrier under ASan/UBSan, and independently adopted the exact baseline merge. LeakSanitizer cannot run in this environment, so local invocations use `ASAN_OPTIONS=detect_leaks=0`; remote gates are unchanged.

The complete production TU and complete exported Windows fixture TU compile with pinned LLVM-MinGW and `-Werror` for x64 and ARM64. The Windows fixture retains all prior ART/read/exit predicates and adds real Bionic, `pthread_create` and `CreateThread` writer kinds, synchronization, overwrite/clear and exit ownership. The existing Windows track requires its new `Bionic concurrent key write contracts: all passed` marker. No workflow, supplier or source-approval receipt is weakened. Existing source-policy and supplier gates pass.

Actual Windows thread registration/adapter execution, the full shim DLL/ART path, NDK guest ELF execution and application performance remain pending genuine full fork CI and subsequent runtime qualification. Compilation and the Linux host boundary do not supply those results.

## Evidence and reproduction

The adjacent [JSON report](pthread-key-assignment-2026-11-23.json) contains every final summary, paired result, first/cold value, exact build/input hash and target command. The [evidence archive](data/pthread-key-assignment-2026-11-23.tar.gz) contains **562 members / 1,271,869 bytes**, including all pilot/final raw receipts and stdout, both ordinary/counted generated sources, final and baseline functional sources/logs, twelve mutants, initial fixture failures, actual Windows compile receipts, the pinned Android reference and independent review evidence. Its SHA-256 is `91cd197cb54fd822949976caeca0d34e5e1aa37e5d70e3b8f5d520ca3ed21ade`; the manifest hashes every other member. Executables/objects are represented by exact hashes and rebuild commands, not bundled binaries.

Reproduce the maintained component from this PR:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tools/test_bionic_key_contracts.py --sanitize --negative-controls
python3 tools/measure_bionic_key_assignment.py --build-only \
  --source-ref 8b3f0bc299ec16141012b1ae2bc2a07358d078cf --output-dir /tmp/set-before
python3 tools/measure_bionic_key_assignment.py --build-only --output-dir /tmp/set-after
python3 tools/measure_bionic_key_assignment.py \
  --baseline /tmp/set-before --candidate /tmp/set-after \
  --rounds 6 --samples 9 --output /tmp/set-abba.json
python3 tools/summarize_bionic_key_assignment.py /tmp/set-abba.json \
  --output /tmp/set-summary.json
```

Run timing with unrelated builds/measurements idle. The helper rejects changed binaries, failed checked values/destructors, wrong counts or process CPU below the thread subtotal. The summarizer can also recompute both archived final blocks together without executing a benchmark. Release acceptance still requires the actual platform/runtime results and an explicit decision about the retained first-use costs.
