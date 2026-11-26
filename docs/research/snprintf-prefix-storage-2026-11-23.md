# Bounded guest snprintf accumulation — 23 November 2026

This is an issue #56 N40/T12 component leaf. It reduces storage and repeated conversion work in the existing guest `snprintf` entry; it does not complete printf compatibility, a whole application performance target, or Windows release qualification.

The old entry builds the entire would-have-written string, even for `snprintf(nullptr, 0, ...)` or a tiny destination. Its large-field helper formats a 128-byte probe, allocates a full temporary, formats again, then copies into the full result string. The new target retains only the caller-visible prefix and separately counts the discarded suffix. A large retained field formats directly into its private result storage. Parsing and argument consumption continue after the prefix fills. Only a valid completed format publishes bytes to the caller.

The ordinary `sprintf`/`asprintf`/FILE and wide formatter targets retain their existing behavior. No cache, thread, socket, lifetime extension or output cap is introduced. The bounded target adds two `size_t` fields to the local result object. Existing unsupported positional behavior, pointer spelling, UTF-32 unit precision and error conventions remain unchanged.

## Repeated complete-call measurements

Baseline: published main `5aa6b39027e9c38de2781ff9cda066418d25b1ba`. Both variants use the same final fixture and compiler options. Six fresh-process ABBA blocks per case provide 12 samples per variant and phase: 216 processes, 432 first-call/steady receipts. CPU is the caller thread clock; wall time is monotonic. Every iteration invokes the complete actual parser/va-list/entry, checks the full count, all retained bytes, NUL and guards, and returns after formatter storage is destroyed. Validation cost is included. Input preparation, process startup and reporting are outside the interval.

These executables use ordinary allocators. Separate builds observe C++ allocation requests; their CPU fields are not used for speed claims. The declared host CRT boundary is glibc, not Windows MinGW/UCRT. All other agent lanes were idle during the final measurement window. All samples, including the earlier pilot and its allocation receipts, are retained in the accompanying JSON.

| Case | Baseline CPU ns/call | Current CPU ns/call | CPU change | Wall change |
| --- | ---: | ---: | ---: | ---: |
| tiny | 111.962 | 111.199 | -0.681% | -1.135% |
| short | 359.444 | 381.882 | +6.242% | +6.387% |
| truncated | 4,476,715.000 | 373,420.781 | -91.659% | -91.657% |
| sizing | 4,208,454.938 | 344,818.719 | -91.807% | -91.813% |
| literal | 699,578.094 | 296,266.625 | -57.651% | -57.658% |
| padding | 725,262.719 | 39,130.961 | -94.605% | -94.609% |
| medium | 238.618 | 212.785 | -10.826% | -10.824% |
| complete | 1,170,116.031 | 319,536.812 | -72.692% | -72.663% |
| mixed | 240,762.840 | 12,871.727 | -94.654% | -94.619% |

Cases: `tiny` formats integer 42 into 8 bytes; `short` formats `-214/payload:feed1234` into 64 bytes. `truncated` and `sizing` format a 4 MiB string into 32 and zero bytes. `literal` has a 4 MiB literal format and 32-byte destination. `padding` formats width 1,048,576 into 32 bytes. `medium` and `complete` retain all 1 KiB and 1 MiB of their string. `mixed` combines a 256 KiB string, literals and integer into 128 bytes.

**Tradeoff and uncertainty:** the short mixed case increases from 359.444 to 381.882 ns/call (+22.438 ns, +6.242%) by the final medians. Its six paired block changes average +1.937%, with a descriptive resampled-block interval of −5.811% to +6.861%; the tiny control is also noisy. The earlier pilot showed a short-case improvement and is retained, not substituted for the final run. Neither a universal small-call improvement nor absence of regression is established. This median crosses the #178 5% investigation trigger; these host samples do not establish an approved release budget. Windows representative-workload measurement and explicit budget acceptance remain open.

## First call in each fresh process

These are the first formatting calls after input preparation, not application cold-start measurements. All 12 observations per variant/case are retained. The short and medium first-call medians increase slightly. No p95/p99 release qualification is inferred from 12 samples.

| Case | Baseline caller CPU µs | Current caller CPU µs |
| --- | ---: | ---: |
| tiny | 15.716 | 15.589 |
| short | 20.652 | 20.971 |
| truncated | 7,183.605 | 485.902 |
| sizing | 5,580.548 | 437.995 |
| literal | 1,994.244 | 337.587 |
| padding | 967.242 | 48.854 |
| medium | 17.337 | 17.529 |
| complete | 1,524.347 | 724.221 |
| mixed | 312.338 | 29.762 |

## Separate allocation receipts

The counters observe C++ allocation requests made during each complete call and verify that observed live storage returns to zero before return. Input/output buffers are prepared before observation. CRT-internal malloc, allocator residency, process RSS/private commit and guest/Windows memory totals are not measured. These are first-call requested bytes and peak live requested bytes; the three-call steady receipts agree per operation.

| Case | Allocations before→after | Requested bytes before→after | Peak live requested bytes before→after |
| --- | ---: | ---: | ---: |
| tiny | 0→0 | 0→0 | 0→0 |
| short | 1→1 | 31→31 | 31→31 |
| truncated | 2→1 | 8,388,611→32 | 8,388,611→32 |
| sizing | 2→0 | 8,388,611→0 | 8,388,611→0 |
| literal | 1→1 | 4,194,305→32 | 4,194,305→32 |
| padding | 2→1 | 2,097,155→32 | 2,097,155→32 |
| medium | 2→1 | 2,051→1,026 | 2,051→1,026 |
| complete | 2→1 | 2,097,155→1,048,578 | 2,097,155→1,048,578 |
| mixed | 3→1 | 1,048,586→128 | 786,440→128 |

The retained result length is at most `n - 1`; this is not a promise that all formatter heap usage is at most `n`. String capacity may grow geometrically, format-specification storage is separate, and the existing UTF-32 `%ls` conversion still creates its UTF-8 temporary. The nine maintained storage budgets cover the declared narrow-string workloads, not those other allocations. Complete output still requires caller-visible storage and parsing still counts every discarded suffix.

## Correctness and qualification

- The actual unchanged baseline and new parser/accessor/entry each pass 280,681 byte/count/guard checks. This count includes individual canary bytes, not 280,681 independent scenarios. The corpus covers capacity boundaries, complete output, register/stack GP and FP arguments, integer lengths, embedded NUL, UTF-8/UTF-32, null strings, widths/precision and existing unsupported syntax.
- Entry errno and both `%m` messages survive large discarded fields. Late invalid widths and precision overflow preserve failure errno and leave every destination byte untouched. Six zero-size `%n` variants and the checked-entry object-size guard retain their precise fatal diagnostics.
- Independent ASan/UBSan replay passes; four compiled production mutants fail their exact count, prefix, NUL or partial-publication assertions. The separate allocation build passes all nine first/steady budgets. No compile error, crash, timeout or missing counter is accepted as a successful negative control.
- The complete production TU and exported Windows Bionic fixture compile under pinned LLVM-MinGW 20261006, x64 and ARM64, with `-Wall -Wextra -Werror`. The shared corpus runs through the actual exported x64 Bionic entry in the existing Windows track. Thirty-eight additional public guest-ABI assertions are wired into the existing x64/translated-ARM64 `libc_test.c` gates. Local cross-compilation is not Windows or guest execution.
- A whole-file pinned reverse transition restores exact published-main `stdio_bionic.cc`, then the unchanged #73 shared-stdio transition restores its preceding source before every original OFD digest is checked. Both layers reconstruct their complete forward endpoints. The original OFD manifest/71 source hashes/patch, #73 manifest/patch, socket core, deadlines and approval flags are preserved. Two agents independently replayed the earlier measurement checkpoint's 32 ordinary, four repinned-property and five repinned-snprintf controls; current-main composition validation is recorded separately below.
- Full `ci.yml` runs the sanitizer/mutant and allocation-budget checks. The branch retains current main's published `92c87bef` reusable framework reference and exact `6dc4cca5` recipe, including all incoming CI and retained-producer acceptance gates.
- Actual Windows runtime and aggregate CI are pending submission. No Windows timing, whole-application gain, private-commit reduction, p95 acceptance or #56/#178 row completion is claimed. Local sanitizer runs set `ASAN_OPTIONS=detect_leaks=0` because this executor prevents LeakSanitizer ptrace; CI retains its normal sanitizer configuration.

## Composition with current main

Before publication, the leaf was composed with main `fdf4861b608d7a5fe8c0fa6c8a285dfbdf573d91`, including #73 shared stream ownership and #321 retained-producer authentication. The measured baseline remains `5aa6b390`; these observations are not relabeled as integrated Windows or whole-product timings. Source comparison proves the baseline and candidate formatter, argument-accessor, snprintf and checked-entry bodies are byte-identical before and after composition. The shared corpus and benchmark fixtures are also identical. The raw JSON records each body hash, the original whole-file identities and the new complete source identity.

The composed production source has SHA-256 `ed233ab2d1744263398a9e8ea8619848574e215fe46cf5751ed0ab5558dbcf9b`. A fresh ASan/UBSan build passes the same 280,681 checks, six zero-size %n diagnostics, object-size diagnostic and four precise compiled mutants. The complete composed production TU and exported Windows fixture both compile strictly for x64 and ARM64; all four commands and results are retained in the raw report. Actual Windows execution remains the full-CI gate.

The composed qualification passes all 32 ordinary transitions, four repinned-property controls, five repinned-snprintf controls, all 24 shared-stdio negative controls and the supplier suite (36 plus one explicit negative case). The shared-stdio suite retains its original reverse-endpoint assertion below the new normalization and additionally rejects an unrelated include through the complete composed chain.

## Reproduction

Use fresh output directories. All source/body hashes, executable hashes, compiler versions/options, observations and statistics are in [the raw report](snprintf-prefix-storage-2026-11-23.json). The baseline uses the same current fixture; production bodies are extracted at strict unique boundaries without rewriting them. The earlier pilot predates only ancillary errno tests and explicit no-argument expected strings; its original hashes remain recorded.

```sh
python3 tools/test_snprintf_bounds.py --sanitize --negative-controls
python3 tools/test_snprintf_bounds.py --observe-allocations --allocation-contracts
python3 tools/test_snprintf_bounds.py --source-ref 5aa6b39027e9c38de2781ff9cda066418d25b1ba --build-output /tmp/snprintf-before
python3 tools/test_snprintf_bounds.py --build-output /tmp/snprintf-after
python3 tools/measure_snprintf_bounds.py --baseline /tmp/snprintf-before --current /tmp/snprintf-after --rounds 6 --output /tmp/snprintf-abba.json
```

For allocation receipts, build both variants again with `--observe-allocations`, then use `measure_snprintf_bounds.py --allocations --rounds 1` with those directories and a new output path. Allocation-observed CPU values are not ordinary allocator timings.
