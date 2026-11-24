# ASCII wide conversion: complete-operation experiment

The production change adds a direct one-byte path in `Wcsnrtombs` after its existing state, input/output-bound and NUL checks. `Wcsrtombs` and `Wcstombs` use the same implementation. It avoids copying an ASCII byte through the temporary multibyte buffer; no new buffer, cache, allocation or lifetime is introduced. Non-ASCII conversion, the pinned encoder's surrogate/U+1FFFFF acceptance, failures, errno, all eight state bytes and source-pointer/NUL consumption stay unchanged.

## Result and retained costs

The fixed six-block ABBA campaign contains **432 fresh processes / 1,728 records** across all18 predeclared cases. ASCII7–65536 conversion medians improve61.997–72.255% in process CPU; every paired block for those five lengths improves. The complete4096-word query/allocate/convert/consume-every-byte/free lifecycle improves **41.778%**, with all six paired blocks improving30.855–50.250%. Restartable, legacy, repeated seven-byte chunks, mixed and invalid-suffix operations retain their complete API results and also improve in these measurements.

**Costs and uncertainty remain part of the result.** The query-only pooled CPU median regresses5.433%, despite five of six paired-block means improving; its sixth block regresses8.003%. One-character conversion has one paired block regressing0.307%. Empty and zero-capacity controls vary substantially between blocks, and the independent pilot's empty-string median regressed42.864% (5.464→7.806ns), while final empty median improves17.140%. The all-non-ASCII row is a control, not a promised new fast path. First-call and p95 results below retain adverse rows and are not app-startup measurements. No release budget or whole-application speed is accepted from this host experiment.

### All final cases

CPU and wall values are per complete scenario operation. A process's steady rate is the sum of its three steady durations divided by the sum of its operation counts; the table uses medians across12 baseline and12 candidate processes per case. Paired-block results use the mean of the two baseline/candidate process rates within each ABBA block. These aggregations can differ when block distributions differ.

| Case | Baseline CPU µs | Candidate CPU µs | CPU change | Wall change | Paired CPU min…max |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | 0.007387 | 0.006121 | -17.140% | -17.144% | -42.516…+12.534% |
| ascii1 | 0.010017 | 0.007295 | -27.170% | -26.922% | -30.091…+0.307% |
| ascii7 | 0.046723 | 0.015362 | -67.121% | -67.101% | -77.394…-46.770% |
| ascii32 | 0.175014 | 0.053984 | -69.155% | -69.150% | -71.524…-44.671% |
| ascii256 | 1.145670 | 0.435385 | -61.997% | -61.996% | -67.021…-59.106% |
| ascii4096 | 19.556647 | 5.426018 | -72.255% | -72.252% | -75.618…-56.384% |
| ascii65536 | 313.019500 | 90.855156 | -70.975% | -70.862% | -73.721…-56.187% |
| measure4096 | 5.701801 | 6.011568 | +5.433% | +5.480% | -25.379…+8.003% |
| restart4096 | 17.446709 | 6.093122 | -65.076% | -65.090% | -78.843…-56.427% |
| legacy4096 | 17.274182 | 6.282153 | -63.633% | -63.647% | -71.703…-53.615% |
| chunks4096 | 20.568442 | 8.361450 | -59.348% | -59.740% | -63.000…-57.264% |
| lifecycle4096 | 30.004389 | 17.469125 | -41.778% | -41.750% | -50.250…-30.855% |
| mixed4096 | 22.415711 | 8.287292 | -63.029% | -63.011% | -73.870…-40.906% |
| nonascii4096 | 26.887570 | 25.251332 | -6.085% | -6.063% | -10.597…+0.978% |
| invalid4096 | 20.437225 | 8.252153 | -59.622% | -59.629% | -67.470…-27.009% |
| output0 | 0.008242 | 0.008003 | -2.897% | -2.897% | -46.737…+23.502% |
| output8 | 0.043166 | 0.019442 | -54.959% | -54.953% | -65.809…-48.752% |
| source4096 | 18.836933 | 5.974069 | -68.285% | -68.242% | -74.013…-58.072% |

### First calls, tails and complete pilot

Each process records its very first production invocation after input/oracle/caller-buffer preparation, then three steady intervals. First-call clock overhead is material for tiny operations. p95 is the nearest-rank observed process rate (12 samples per variant in the final), not a statistical population guarantee. Full subprocess wall, including setup and output verification, is retained separately in JSON.

| Case | First CPU change | First wall change | CPU p95 baseline→candidate µs | Wall p95 baseline→candidate µs | Pilot steady CPU change |
| --- | ---: | ---: | ---: | ---: | ---: |
| empty | +12.310% | -0.380% | 0.017298→0.007961 | 0.017343→0.007965 | +42.864% |
| ascii1 | -4.056% | -3.867% | 0.015943→0.013512 | 0.015945→0.013515 | -41.889% |
| ascii7 | -17.276% | -5.836% | 0.079126→0.029456 | 0.080092→0.029467 | -67.288% |
| ascii32 | -11.999% | -6.906% | 0.240813→0.180883 | 0.240809→0.180886 | -73.246% |
| ascii256 | -17.385% | -10.110% | 1.591780→0.582775 | 1.594607→0.582836 | -74.207% |
| ascii4096 | -64.498% | -60.191% | 25.041255→10.496208 | 25.072503→10.497261 | -82.211% |
| ascii65536 | -75.977% | -75.727% | 419.666917→161.234625 | 420.450417→161.252104 | -66.530% |
| measure4096 | +1.221% | +2.141% | 9.343099→7.302654 | 9.348224→7.307790 | -46.048% |
| restart4096 | -66.766% | -62.648% | 25.657773→9.365838 | 25.674904→9.371454 | -69.304% |
| legacy4096 | -65.595% | -61.154% | 28.129786→8.383675 | 28.131255→8.384337 | -68.686% |
| chunks4096 | -57.638% | -53.407% | 31.824407→13.054620 | 31.884186→13.058070 | -50.513% |
| lifecycle4096 | -39.037% | -37.443% | 39.577237→22.786053 | 39.576980→22.803495 | -46.985% |
| mixed4096 | -63.908% | -60.549% | 38.898833→13.784719 | 43.516299→13.785766 | -59.543% |
| nonascii4096 | +8.273% | +7.523% | 40.221936→31.855645 | 43.560594→31.977059 | -9.121% |
| invalid4096 | -52.613% | -49.178% | 25.713841→16.201838 | 25.749613→16.202741 | -48.905% |
| output0 | +3.527% | +4.102% | 0.013207→0.010864 | 0.013209→0.010866 | -12.764% |
| output8 | -16.166% | -11.575% | 0.069854→0.026310 | 0.069930→0.026311 | -75.708% |
| source4096 | -67.561% | -63.300% | 27.200678→8.840079 | 27.624290→8.841656 | -72.742% |

## Source, qualification and resources

- Baseline: `73bf018b8c02228ac1e5ad4ce4656296674b07ed`, whole locale source SHA-256`d0fc12e8bc6c95e292ebcf3a2541e6ee57de6ef9f6d8c2a872ea032840682e92`. Earlier730d baseline qualification is reusable only because that whole file is byte-identical; subsequent73bf contains four unrelated BLE paths.
- Frozen measured implementation/caller/contracts: local commit`9b3947bc6530a84ab8ed96d6e7b645848e2b7aae`, tree`49e265ecac58572fc7aa89b467951716fcc2de98`. Candidate whole locale SHA-256`77b803efa81e0606ea0f927bd46829a059d7c9828f9afc8699d8668512b9e772`. The archived Git bundle preserves that exact source commit with its stated main prerequisite. Later documentation/evidence-gate changes are a separate publication phase, not a different measured implementation.
- Both actual baseline and candidate pass **8,537,842** production/pinned-reference/real-guard checks under GCC ASan/UBSan, plus the complete **1,748** maintained pinned assertions. The candidate also passes ordinary LLVM23 and default-GCC sanitizer qualification. Pinned Android16 source digests are validated by the existing reference preparer; supplier function bodies are unchanged. Shared1500 additional public-call assertions are included in the real NDK/native2 contract entry.
- **Ten separately compiled actual-source negatives** fail their intended semantic assertions: ASCII boundary/count/source progress, pending-state handling, reserved bytes, NUL consumption, count-only source position, encoder asymmetry, partial multibyte output and zero output bound. Compile errors are never counted as negative passes.
- Complete baseline/candidate Windows x64 and ARM64 locale translation units compile with the pinned LLVM toolchain, warnings as errors. The initial recipe failed because the existing focused workflow omitted its newly required ARM64 header include path; the minimal include-path fix and the failed attempt are retained. No Windows execution is claimed.
- Timing uses one common caller object and separately compiled unchanged production UTF bodies, ordinaryLLVM23 `-O2`, no LTO and no sanitizers/resource counters. Undefined conversion references in the caller object are recorded. Ordinary callers check complete output after each interval and accumulate each call's return/source/errno/all-state fingerprint; only the full lifecycle hashes every output byte on every call. The standalone eight-million-check differential is separate from timing.
- All18 separately instrumented resource pairs are identical. Every conversion-only operation performs zero scoped C++ allocations. The complete lifecycle allocates/frees exactly one4097-byte buffer, peak4097 requested bytes, retained0. This is neither total heap nor RSS; caller input/reference preparation is outside the scope. Actual conversion objects have no unresolved allocator references. Stack frames also remain identical: Linux bounded72B/legacy24B; Windows x64 bounded88B/legacy24B; ARM64 bounded112B/legacy32B; restartable wrappers0B in these builds.
- Pilot: `2026-11-23T23:46:55.374210+00:00`–`2026-11-23T23:46:57.640833+00:00`,72 processes/288 records. Final: `2026-11-23T23:47:58.474534+00:00`–`2026-11-23T23:48:12.228635+00:00`,432 processes/1728 records. All504 processes/2016 records, every stdout/stderr, frozen predeclaration and full subprocess wall survive. Agent CPU work was paused; the shared executor was not claimed to be an isolated or pinned machine. No timing run was discarded or repeated.
- Earlier failed compiler preparation attempts are retained: whole-block unused leaves and unchanged pinned-source GCC warnings were made explicit before measurement; the two supplier warnings remain visible with GCC-specific nonfatal diagnostics, and supplier bodies were not rewritten. The default `c++` compiler is identified by its actual version output.

## Evidence and reproduction

The adjacent JSON contains full-precision results, every paired block, both campaigns, source/build/resource/stack receipts and environment boundaries. Archive: [`data/wide-ascii-2026-11-23.tar.gz`](data/wide-ascii-2026-11-23.tar.gz), **5,119,878 bytes / 1,455 members**, SHA-256`0689ee22c8755627aad1d9455962f426dc3266a2a100ff3debbd52b8112661a0`. Its internal manifest binds every retained source, binary, object, raw observation and failed preparation. The archive includes the frozen source bundle; it never substitutes for a real platform run.

```sh
python3 tools/test_wide_ascii_contracts.py --sanitize
python3 tools/test_wide_ascii_performance_evidence.py
# To create a new separately labeled experiment, not overwrite this evidence:
python3 tools/bench_wide_ascii.py prepare --baseline-ref 73bf018b8c02228ac1e5ad4ce4656296674b07ed --compiler /path/to/clang++ --out-dir /tmp/wide-build-new
python3 tools/bench_wide_ascii.py run --build-dir /tmp/wide-build-new --out-dir /tmp/wide-pilot-new --blocks 1 --label pilot
python3 tools/bench_wide_ascii.py run --build-dir /tmp/wide-build-new --out-dir /tmp/wide-final-new --blocks 6 --label final
```

The performance evidence verifier is mandatory in fullCI. Full fork workflow dispatch and actual Windows/Android execution remain separately reported receipts. This bounded conversion improvement contributes to issue56; it does not close a broad roadmap row or performance-release criterion.
