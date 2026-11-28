# Same-size RGBA input conversion

This is a measured software-media component of [#56 T12](https://github.com/gesher-runtime/gesher/issues/56), not completion of the real-app performance-budget row. The baseline is `5aa6b39027e9c38de2781ff9cda066418d25b1ba`. Only `RgbaToI420` production code changes: input and output with the same dimensions use direct source rows instead of the nearest-neighbour coordinate mapper. The scaled path remains byte-identical.

The actual `Codec::OnSurfaceFrame` and `Recorder::OnFrame` call this converter after allocating an owned `VideoFrame`. Their input Surfaces normally have the configured encoder dimensions. The new path keeps the original coefficients, integer rounding, alpha handling, top-left chroma selection, odd dimensions, destination strides, padding, metadata and Y-then-UV pass order. The latter also preserves the existing behavior when source and destination storage overlap. No new allocation, retained storage, thread, queue policy or timing rule is introduced.

## Measured complete component workloads

Each interval includes creation of the actual software AVC encoder, every actual input callback's allocation/conversion/queueing, queue removal, real provider encoding, drain, destruction and packet collection. It excludes source-pattern generation, input hashing, the subsequent scalar-reference encoder, file writing and independent FFmpeg decoding. The callback bodies come from production source, including their real mutexes and bounded queues; the fixture supplies surrounding state and explicit input clock values. It does not execute JNI dispatch, a Surface producer, camera/GPU capture, asynchronous worker dispatch, muxing or ART. These are complete component workloads, not end-to-end application measurements.

Eight samples per revision were collected in four ABBA groups, one fresh process for each sample. All 16 processes and all nine workloads per process are retained in [the raw JSON](rgba-i420-same-size-2026-11-23.json). There are no warm/cold or outlier exclusions. The actual encoder runs before its scalar-reference encoder in each workload, so the first actual provider instance is included rather than warmed by its reference. This does not establish a cold filesystem, cold OS or cold hardware state.

| Actual input callback and workload | CPU before / after, ms | CPU change | Wall before / after, ms | Wall change |
| --- | ---: | ---: | ---: | ---: |
| Codec, 320×240, 12 frames | 24.659 / 20.859 | −15.41% | 13.972 / 11.335 | −18.87% |
| Recorder, 641×479 → 320×240, 12 frames, scaled control | 27.457 / 26.468 | −3.60% | 15.836 / 15.648 | −1.19% |
| Codec, 1280×720, 24 frames | 447.357 / 382.254 | −14.55% | 229.539 / 161.642 | −29.58% |
| Recorder, 1280×720, 24 frames | 442.546 / 379.393 | −14.27% | 220.913 / 157.658 | −28.63% |
| Codec, 1920×1080, 8 frames | 370.025 / 309.325 | −16.40% | 175.788 / 122.934 | −30.07% |
| Recorder, 1920×1080, 8 frames | 360.392 / 303.158 | −15.88% | 168.522 / 120.301 | −28.61% |

Numbers are medians, not a bound on each invocation. The scaled control still executes the original mapper; its small timing difference is treated as noise, not an optimization result. Complete-stream ranges overlap, and the raw data includes substantial scheduling variance: the baseline Recorder 1080p CPU samples range from 320.195 to 777.648 ms, versus 264.894 to 439.085 ms for the candidate. The small first workload is retained even though its short duration makes relative variability larger.

The uninstrumented converter-only intervals include one warm-up before timing, use an already allocated destination, and verify the complete output afterward:

| Same-size conversion workload | CPU before / after, ms | CPU change | Wall before / after, ms |
| --- | ---: | ---: | ---: |
| 1280×720, 30 frames | 127.858 / 49.130 | −61.57% | 127.857 / 49.706 |
| 1920×1080, 20 frames | 186.015 / 68.121 | −63.38% | 186.063 / 68.114 |
| 3840×2160, 8 frames | 299.029 / 108.661 | −63.66% | 299.081 / 108.658 |

These Linux results use GCC 14.2, `-O2` for the wrapper/probe, and the actual pinned AVC provider at `-O3` with its x86/SSE4.2 configuration and AVX2 disabled. Both revisions link the same provider library. The raw report records the optimization/feature flags, source hashes, provider hash, CPU model, affinity, clocks and samples. Production converter and encoder are separate translation units, preventing the test caller from constant-folding the baseline's dimensions. Timed builds have neither the coordinate observer nor sanitizers. Process CPU time includes provider worker threads; wall time uses `steady_clock`.

There is no Windows speed, real-app frame-rate, power, RSS or allocation reduction claim. The host compiler and provider build are not substitutes for the production Windows build. Existing frame allocations and their lifetimes are unchanged.

## Deterministic work budget

The correctness build adds an observer only to the generated test copy of the old mapping lambda. The actual source in the product is uninstrumented. It counts mapping calls, not machine division instructions, which a compiler may hoist or otherwise transform.

| Six real-provider workloads | Baseline mappings | Candidate mappings |
| --- | ---: | ---: |
| Codec 320×240, 12 frames | 1,152,000 | 0 |
| Recorder scaled control, 12 frames | 1,152,000 | 1,152,000 |
| Codec 720p, 24 frames | 27,648,000 | 0 |
| Recorder 720p, 24 frames | 27,648,000 | 0 |
| Codec 1080p, 8 frames | 20,736,000 | 0 |
| Recorder 1080p, 8 frames | 20,736,000 | 0 |
| Total | 99,072,000 | 1,152,000 |

The gate requires zero mappings for the eligible geometry and the exact original count for scaled geometry. It uses no host-speed threshold.

## Correctness and ownership evidence

`tests/media/rgba_i420_test.cc` compiles the complete production converter and software AVC encoder as separate translation units. `tools/test_rgba_i420.py` extracts the complete production Codec/Recorder input callback bodies and Recorder clock method into explicit fixture containers. Only surrounding metadata/clock boundaries and POSIX aligned allocation are adapted. Unused decoder-selection and metadata functions abort rather than returning fabricated successful results.

The independent scalar oracle expresses chroma rounding with a positive numerator and division, avoiding a duplicate signed-right-shift implementation. It preserves the original nearest-neighbour/top-left selection and compares every byte, including padding. The corpus covers:

- All 16,777,216 RGB triples, varied alpha, and different neighboring colors that detect chroma averaging or selection of the wrong pixel.
- 407 geometry cases: same-size, up/down scaling, odd widths/heights, narrow rows, and three destination-padding values. Source guards, complete destination padding, and all frame metadata are checked.
- Six invalid-dimension no-op cases and four source/destination overlap offsets, preserving the original order of reads and writes.
- Codec pre-start/EOS rejection, bounded-queue oldest-drop behavior and fallback timestamps; Recorder stopped/paused/pre-start/pre-resume rejection, drop accounting and increasing presentation timestamps.
- Six complete streams through the actual callbacks and pinned libavc: 88 actual frames and 88 independently converted reference frames. Every encoded byte and packet's configuration/key/PTS fields agree. All original RGBA inputs remain intact. Retained consumer frames remain intact after later encoding, drain and encoder destruction.
- FFmpeg 7.1.5 independently decodes all 88 actual frames. Baseline and candidate bitstreams and full-frame hashes agree, and every expected frame is present.

The final candidate ASan/UBSan corpus passes **68,727 checks**, plus all-RGB comparisons, all 88 FFmpeg frames, and four assertion-specific negative controls. The untouched baseline passes **68,583 checks** in the full reference mode, which disables only the new same-size mapping budgets; scaled budgets remain enforced. Running the original source under the candidate budget fails `same-size mapping budget` as intended. The differing check count is the disabled efficiency assertions, not a smaller baseline input corpus.

| Negative control | Required failure |
| --- | --- |
| Disable the same-size path | `same-size mapping budget` |
| Remove the source-height equality guard | Complete output and padding oracle |
| Read the wrong chroma column | Complete output and padding oracle |
| Write one column into destination padding | Complete output and padding oracle |

Each mutant fails its specified assertion, with no sanitizer error substituting for the oracle. The first mutant fixtures deliberately have padded destinations, so the padding-write mutant is detected as a semantic error without an out-of-bounds write.

An independent reviewer replayed the real-provider ASan/UBSan corpus and all four controls successfully. The final CI-shaped replay also rebuilds the real provider from the pinned source instead of depending on the reviewer's cached library. The only subsequent test-order change runs each actual encoder before its scalar reference; the final replay and all reported timings use that order. The production converter is unchanged between those runs.

Local LeakSanitizer was disabled because this executor's ptrace environment prevents it from operating. ASan and UBSan remained active. The committed CI command retains normal sanitizer settings. The report does not infer a leak test from the local run.

## Build and CI boundaries

The complete converter, software encoder and fixture translation units compile with strict warnings for Windows x64 and ARM64. The x64 fixture also links against the actual provider library. Windows execution remains a required full-CI result, not a locally completed result.

The PR adds the complete sanitized real-provider corpus and four mutants to the existing Linux preflight, and makes `media_rgba_i420_test.exe` mandatory in the existing Windows media track. No original assertion, timeout or gate is relaxed. The one reusable-workflow pin repair selects published main `5aa6b39` instead of the inaccessible `fe42fc3` commit; both contain exactly the same reviewed `framework.yml` blob, `6dc4cca565aaf15fdf518eee825b0e44765b275f`. The new recipe and hosted-runner choice are preserved.

## Reproduction

Use the libavc checkout from `aosp.lock`, currently Android `android-16.0.0_r4`, commit [`d380d04a78c6c08702e36aca86d0fd4baee390f9`](https://android.googlesource.com/platform/external/libavc/+/d380d04a78c6c08702e36aca86d0fd4baee390f9). The helper verifies the checkout/tag and clean tracked source. FFmpeg must be available for independent decoding.

```sh
python3 tools/test_rgba_i420.py \
  --libavc-root third_party/aosp/external/libavc \
  --sanitize --negative-controls --output-dir /tmp/rgba-candidate

python3 tools/test_rgba_i420.py \
  --libavc-root third_party/aosp/external/libavc \
  --source-ref 5aa6b39027e9c38de2781ff9cda066418d25b1ba \
  --sanitize --measure-only --output-dir /tmp/rgba-baseline
```

For timing, build without sanitizers or observers using `--measure`, with separate baseline/candidate output directories. Build the provider once and pass its same `libavc-rgba-test.a` with `--libavc-library` to both invocations. `--source-ref` selects all four production input files; the baseline and candidate source receipts differ only in the converter. After both builds complete, repeatedly execute each generated `rgba-i420-test measure` binary in ABBA order. Passing an output directory as its next argument also saves the actual/reference bitstreams. The helper's `decode_outputs` validates their byte identity and independent full-frame decode. The JSON includes every raw `STREAM`/`CONVERT` line and all original sample timings, allowing recomputation of the tables without rebuilding or selecting samples.
