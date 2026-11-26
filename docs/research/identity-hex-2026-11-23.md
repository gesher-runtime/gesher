# Exact lowercase SHA-256 identity serialization

## Result and acceptance boundary

The change removes repeated general-purpose formatting from three existing identity producers. Complete measured short shortcut calls use **65.17–70.40% less caller CPU**, signed notification identity calls **67.76–89.90% less**, and an uncached AppOps signer for a new owner **78.44% less**. Their output bytes, digest inputs, identity semantics and existing caches are preserved. Actual unsigned file identity calls also improve in the measured host boundary.

The complete shortcut Properties publication/readback/removal control has **50.22% less caller allocation**, while caller CPU is effectively unchanged (**−0.62%**) and elapsed median increases **3.68%**. This report does not promote the earlier pilot's larger publication CPU result.

There are material acceptance qualifications. The unchanged signed cache-hit control has a **+11.90% median caller CPU observation, +3.905 ns/call**, despite zero allocation in both variants and unchanged hit-path code. Several first-call p95 observations are worse, including Properties publication CPU **63.270 → 105.623 ms**. All are retained below. These measurements **do not establish a universal 5% budget pass, cold-start improvement, or release acceptance**. Actual Windows/ART, application-level startup, heap/retained-state, power and the complete CI aggregate remain separate requirements.

This is a partial **#56 T12** efficiency leaf supporting identity work used by **F08 AppOps, F32 notifications and F51 shortcuts**. It does not close those functional rows or #178's wider application budgets; it does not implement JAVA-07's image/publication redesign.

## Production scope and preserved contracts

Only three expressions change in the complete production classes:

1. `AppShell.dynamicKey` encodes the existing SHA-256 digest with `libcore.util.HexEncoding.encodeToString(bytes, false)`.
2. `NotificationManagerService.codeIdentity` retains `apk:`/`cert:` selection and encodes the existing final digest with the same lowercase helper.
3. `AppOpsModeStore.computeSigner` encodes each existing certificate digest with that helper before the unchanged sort and colon join.

The previous loop invokes `String.format(Locale.ROOT, "%02x", byte & 255)` for each of the 32 digest bytes. The shipped specialized helper writes two hexadecimal characters per byte. Its `false` parameter selects lowercase, as specified by the [pinned Android 16 libcore source](https://android.googlesource.com/platform/libcore/+/android-16.0.0_r4/luni/src/main/java/libcore/util/HexEncoding.java). That complete source has SHA-256 `010821679f96f23de62cb22915d4896ff6f010d3bc6eeb3eb188e283622b714b`. The actual supplied `HexEncoding.class` is hashed independently in every compiled manifest; source provenance is not inferred from a matching class name.

No digest algorithm/input, package validation, UTF-8/NUL framing, prefix, certificate ordering, duplicate semantics, sorted signer list, filename/content framing, exception boundary, caller permission, or owner/cache lifecycle is changed. Notification identities hash the **ordered concatenation of length-framed certificates**; AppOps identities hash certificates separately and sort the encoded values. Those remain deliberately different contracts. There is no new per-owner cache, index, worker or permission decision.

The final overlap audit read actual file lists at #332 `5d64cc5779b6e73ee8b9c0db6c7247fb9e562cb0` and #266 `6c43bd3b5c2f5348c5fcd5b129ff58ab3118ee8b`; neither changes any of these three producers. The audit receipt is in the evidence archive.

## Complete-source qualification

The maintained host entry compiles **all three complete production classes**, and the unchanged baseline classes from exact main `b2e1d97813e65e58d7e6dbd10ea21d1f821e405d`, against the same real app/framework/core inputs. It does not extract or replace the hashing, framing, cache, file I/O or publication methods. Java compilation uses the app's source/target 17 and inline string-concatenation convention.

The default contract corpus passes **1,374 checks** on both complete variants, plus actual shortcut publication/readback/removal. It checks all 256 byte values against the shipped lowercase encoder; exact empty/UTF-8/NUL/Unicode/surrogate/large IDs under five locales; invalid package handling; 256 complete signed identities covering every digest byte; empty/null/multiple/reordered/duplicate signers; fresh package-owner isolation and cached String reference identity; actual sorted base/split contents; same-size replacement, filename changes, missing/unreadable bases and unsigned cache behavior. Golden digests use independently framed inputs and fixed-width BigInteger formatting. Benchmark golden strings are calculated with Python hashlib before the JVM starts, and every timed complete result must match.

Three separately compiled production mutations change only the intended lowercase flag to uppercase. Each is rejected at its exact shortcut, notification or AppOps output assertion and cannot emit the completion sentinel. Full application and services Java/D8 builds pass; the application boot-link check reports **292 references, zero unresolved**. Existing AppOps contracts pass **489 checks**, and notification delegation passes **260 checks in each protected mode**.

An independent agent replay passes the same final **1,374 checks and real publication lifecycle** and verifies the direct AppOps host bridge. The Windows DEX fixture retains reflection for private boot-class access. Only the host timing caller uses `IdentitySignerHost`, a single ordinary same-package/same-loader call to the real `AppOpsModeStore.signer`; it adds no reflection or substituted cache/policy inside the timed path. Selected, supplier and vector preflights pass with all original whole-file manifests and negative controls intact. The framework host gate and Windows 90-second complete contract gate are added; existing gates are preserved.

Installed-signature inputs are constructed pre-admitted metadata with real Signature objects. APK verification/package admission is outside this fixture. The declared host edges are the existing executable-path JNI shim and locale fields because the host API archive lacks the Windows locale/Android ICU environment. Properties publication uses `GESHER_SHORTCUTS=0`; Windows Shell link creation is excluded. No remote Binder, ART or Windows runtime result is inferred from these host controls.

## Fixed final measurement method

Environment: `Linux-6.18.57-x86_64-with-glibc2.41`, reported CPU `INTEL(R) XEON(R) PLATINUM 8573C`, Temurin OpenJDK 21.0.13.1+2. This is a shared hosted Linux environment. The three other working lanes explicitly held their CPU work during each declared block; finite work between blocks was allowed and timestamped. This is not a dedicated-machine benchmark.

The final campaign contains **six ABBA blocks, 312 fresh JVMs and 1,248 observations**: all 13 cases, baseline/candidate/candidate/baseline, one first-call record plus three measured samples per JVM. No failed/slow process or sample was removed; all outputs and file retirement predicates pass. Final classes, source, inputs, case counts and default-VM command stay frozen throughout. Flags are `-XX:-UsePerfData -Xms64m -Xmx256m`; **the final campaign does not use `-Xbatch`**.

For each JVM, steady costs are the **sum of all three sample costs divided by all measured calls**. Thus measured WeakHashMap growth/JIT samples remain charged. Tables compare the median of the resulting **12 independent JVM values per variant/case**; the 36 individual samples are not treated as independent runs. The JSON includes every value, mean, range, linearly interpolated descriptive p95 and all six paired block means. First calls are summarized separately.

CPU uses the calling thread's CPU counter. Allocation uses that thread's allocated-byte counter. Elapsed covers the complete call loop. Identical complete-output comparison and result consumption remain inside the clocks. These counters exclude background JIT/GC threads, fixture preparation, JVM startup, process RSS and retained heap; they are not total-process CPU or memory. File inputs are created before clocks, so the reads include real I/O APIs and content hashing but do not represent cold storage.

“First” means the first call **after fixture preparation/class discovery and fixed input construction**, not process launch or APK installation. Prepared owners are held for the entire new-owner JVM, so no owner disappears from the weak map during sampling. Every prepared owner must be processed exactly once. File/publication cleanup is required before the completion marker; process-scoped owners and the existing weak cache end with the observed JVM exit. Cache reclamation latency is not measured.

| Case | Complete operation and fixed input | Warmup / measured calls per sample |
| --- | --- | ---: |
| `key-tiny` | Validated shortcut arguments, SHA-256 of package + NUL + empty ID, exact lowercase key and arguments | 20,000 / 5,000 |
| `key-unicode` | Same complete operation with Hebrew, emoji, quote and embedded NUL | 20,000 / 5,000 |
| `key-large` | Same complete operation with an 8,192-character ID | 10,000 / 2,000 |
| `signed-one` | Real codeIdentity over one pre-admitted 1,024-byte certificate and ordered length framing | 20,000 / 5,000 |
| `signed-four` | Real codeIdentity over four ordered 1,024-byte certificates | 20,000 / 5,000 |
| `signed-empty` | Real empty but non-null certificate-array identity; preserves cert: framing | 20,000 / 5,000 |
| `unsigned-tiny` | Real base.apk filename/length/content hashing and file close, 257 bytes | 2,000 / 500 |
| `unsigned-splits` | Real sorted base.apk and two split files, 65,537 + 31 + 8,193 bytes | 1,000 / 100 |
| `unsigned-large` | Real 1 MiB base.apk read/hash/close | 128 / 16 |
| `appops-new-owner` | Real uncached signer for each distinct pre-admitted owner, two certificates (1,024 and 257 bytes), sorting/join/cache publication | 5,000 / 512 |
| `appops-cached` | Same real signed owner after cache warmup; unchanged hit-path control | 250,000 / 100,000 |
| `appops-unsigned` | Same real unsigned owner after cached-null warmup; unchanged hit-path control | 250,000 / 100,000 |
| `shortcut-publication` | Real empty shortcut Properties publication, stored publisher readback, authorized load, removal and absence check | 64 / 32 |

## All steady complete-call results

Negative changes mean a reduction. CPU and allocation values are per call; elapsed changes use the same complete-call boundary. Zero-allocation controls have no meaningful allocation percentage.

| Complete case | Caller CPU µs/call, baseline → candidate | CPU change | Allocated B/call, baseline → candidate | Allocation change | Elapsed change |
| --- | ---: | ---: | ---: | ---: | ---: |
| `key-tiny` | 9.406 → 2.785 | -70.40% | 20,592.00 → 3,904.71 | -81.04% | -70.23% |
| `key-unicode` | 8.123 → 2.829 | -65.17% | 21,488.00 → 4,152.74 | -80.67% | -64.96% |
| `key-large` | 14.897 → 12.147 | -18.46% | 36,964.31 → 28,553.45 | -22.75% | -19.36% |
| `signed-one` | 8.973 → 1.652 | -81.59% | 19,160.00 → 2,047.13 | -89.32% | -80.65% |
| `signed-four` | 9.962 → 3.212 | -67.76% | 22,592.00 → 5,288.31 | -76.59% | -67.45% |
| `signed-empty` | 6.999 → 0.707 | -89.90% | 18,080.00 → 1,087.79 | -93.98% | -90.76% |
| `unsigned-tiny` | 72.864 → 24.649 | -66.17% | 88,808.00 → 68,320.09 | -23.07% | -65.64% |
| `unsigned-splits` | 133.967 → 81.108 | -39.46% | 91,528.00 → 70,992.00 | -22.44% | -42.71% |
| `unsigned-large` | 1,156.096 → 1,018.073 | -11.94% | 88,536.00 → 68,064.00 | -23.12% | -10.24% |
| `appops-new-owner` | 24.868 → 5.363 | -78.44% | 38,275.62 → 3,746.97 | -90.21% | -79.66% |
| `appops-cached` | 0.033 → 0.037 | +11.90% | 0.00 → 0.00 | n/a | +11.86% |
| `appops-unsigned` | 0.032 → 0.031 | -2.02% | 0.00 → 0.00 | n/a | -2.00% |
| `shortcut-publication` | 1,063.497 → 1,056.859 | -0.62% | 285,110.88 → 141,914.50 | -50.22% | +3.68% |

The signed AppOps cache control is **32.807 → 36.712 ns/call**, **+3.905 ns**. All final cache samples allocate zero bytes. The unchanged `signer()` bytecode matches after constant-pool-number normalization; the complete proof is retained. That source evidence excludes a new lookup, lock or encoder on the hit path, but does not establish the cause of the measured timing change. Paired block CPU deltas range **−19.19% to +41.97%**; pooled process means are **36.971 → 37.845 ns (+2.36%)**. Neither statistic is substituted for the adverse median or used to declare a budget pass. The unsigned cached-null control is **−2.02%**.

## All first-call and tail observations

Each cell is baseline → candidate. The sample size is only 12 fresh JVMs per variant/case; p95 is a descriptive interpolation near the observed extremes, not a reliable population-tail guarantee. Its adverse values are still retained rather than dismissed.

| First call after fixture preparation | CPU median ms | CPU p95 ms | Elapsed median ms | Elapsed p95 ms | Allocation B |
| --- | ---: | ---: | ---: | ---: | ---: |
| `key-tiny` | 24.868 → 21.319 | 30.547 → 25.846 | 25.472 → 21.651 | 31.922 → 26.072 | 652,472 → 636,136 |
| `key-unicode` | 20.678 → 19.229 | 24.708 → 26.005 | 21.379 → 19.621 | 25.356 → 26.366 | 652,688 → 636,384 |
| `key-large` | 23.044 → 22.216 | 26.555 → 26.455 | 23.610 → 22.799 | 27.170 → 26.905 | 677,072 → 660,752 |
| `signed-one` | 23.155 → 20.480 | 26.670 → 22.976 | 23.601 → 20.518 | 26.971 → 23.130 | 783,288 → 726,504 |
| `signed-four` | 24.863 → 24.006 | 29.861 → 38.688 | 25.645 → 24.065 | 30.264 → 39.287 | 786,720 → 729,872 |
| `signed-empty` | 23.485 → 21.688 | 26.521 → 35.912 | 24.029 → 21.791 | 26.821 → 36.812 | 782,048 → 725,216 |
| `unsigned-tiny` | 26.534 → 22.966 | 30.439 → 45.455 | 26.832 → 23.192 | 31.534 → 45.998 | 921,928 → 865,192 |
| `unsigned-splits` | 31.844 → 33.042 | 40.066 → 44.623 | 33.332 → 33.259 | 44.787 → 44.830 | 924,696 → 867,912 |
| `unsigned-large` | 54.504 → 45.385 | 67.190 → 71.996 | 54.890 → 45.569 | 67.846 → 72.196 | 921,880 → 865,160 |
| `appops-new-owner` | 19.412 → 16.289 | 22.169 → 26.046 | 19.678 → 16.539 | 22.826 → 26.218 | 401,552 → 324,128 |
| `appops-cached` | 19.105 → 15.361 | 21.678 → 23.266 | 19.792 → 15.598 | 23.262 → 25.055 | 401,272 → 324,128 |
| `appops-unsigned` | 0.042 → 0.043 | 0.047 → 0.081 | 0.039 → 0.040 | 0.044 → 0.077 | 760 → 760 |
| `shortcut-publication` | 52.315 → 50.304 | 63.270 → 105.623 | 53.721 → 51.751 | 65.074 → 107.105 | 1,760,800 → 1,602,088 |

First-call median CPU is worse for split-file identity (**+3.76%**) and the very short unsigned AppOps control (**+1.67%**). Tail observations are substantially worse in several cases: Properties publication CPU p95 **+66.94%**, small unsigned-file identity **+49.33%**, empty signed identity **+35.41%**, and unsigned AppOps **+71.41%** (about **47.306 → 81.087 µs**). Elapsed tails also remain in the table. These observations leave cold/startup/tail acceptance unresolved; no threshold was invented to mark them green.

## Earlier diagnostics retained separately

The first default-VM pilot used a reflective AppOps timing adapter and shorter cache warmup. Its cache control showed +42.64% median CPU, and reflective per-call allocation disappeared only in later samples. A separately approved fixed 12-JVM `-Xbatch`/250,000-warmup diagnostic retained zero cache allocation but still showed an adverse cache median (+11.23%, +2.84 ns). This was a changed VM configuration, not a production result.

The final direct caller removes that artificial reflection boundary while preserving the real signer and owner lifecycle. All earlier 52-JVM pilot and 12-JVM diagnostic results, source versions, commands, first-call adverse observations, and full smoke counters remain in the archive. They are not pooled with the final 312 JVMs or used to replace an unfavorable final result. No further tuning or timing variant is planned from the final data.

## Source identity and current-main integration

| Complete production path | Baseline SHA-256 | Candidate SHA-256 |
| --- | --- | --- |
| `app/java/com/gesher/app/shell/AppShell.java` | `8864cb42e844f7e7cad1a5285533a94f35f6857ee69fc822c77bb2c40f764aeb` | `b4f8744ba50d19d35092442f0dc83b7d672048179626accc93379609f6e06c6d` |
| `app/java/com/gesher/server/NotificationManagerService.java` | `97c0331485a91d5919a1ef766b83bc0252f7bb43ef37bf28fab04385e5706527` | `21f715bb64eaefa32e81fc731ba924b8a56e52c603ac029146ee738befccba90` |
| `app/java/com/gesher/server/AppOpsModeStore.java` | `4b8fe31c09b471785adb6543628285a098a9402a0785d1fe2c9410bfd47b8f26` | `3eace9f02d2825bac5281dead681dc19c71f4688d129877cc21f7ea4de785171` |

After the frozen campaign, current main `d94ad6025ac79e2019026caa0bca6a4b9f8adca9` was integrated as an exact fast-forward from b2e1. Its nine incoming #74 paths do not touch these producers, fixtures, callers or gates; every incoming blob is preserved. The complete uncommitted feature diff is byte-identical across the integration. All **21 generated candidate class files** remain byte-identical when recompiled against the newly rebuilt current-main app jar. Current-main application/services DEX, 1,374 complete contracts plus publication, and selected/supplier/vector gates pass. The historical measurements retain their exact b2e1/common-jar provenance; no new performance claim is inferred from integration.

| Final block | UTC start | UTC end |
| --- | --- | --- |
| 1 | 2026-11-23T18:30:54.772367+00:00 | 2026-11-23T18:31:20.959068+00:00 |
| 2 | 2026-11-23T18:31:56.069577+00:00 | 2026-11-23T18:32:21.950189+00:00 |
| 3 | 2026-11-23T18:33:38.939488+00:00 | 2026-11-23T18:34:04.378764+00:00 |
| 4 | 2026-11-23T18:35:05.442745+00:00 | 2026-11-23T18:35:30.391913+00:00 |
| 5 | 2026-11-23T18:36:05.617524+00:00 | 2026-11-23T18:36:29.999316+00:00 |
| 6 | 2026-11-23T18:37:01.403673+00:00 | 2026-11-23T18:37:29.064544+00:00 |

## Reproduction and evidence

The maintained entry is [`tools/test_identity_digests.py`](../../tools/test_identity_digests.py). Supply the JDK, framework/core class directories, actual app classes jar and apksig jar. Compile baseline with `--source-ref b2e1d97813e65e58d7e6dbd10ea21d1f821e405d --compile-only --work-dir BASELINE`; compile candidate without `--source-ref` into a separate directory. Run the complete contracts before any measurement. The three `--mutation` values are `key-uppercase`, `notification-uppercase` and `appops-uppercase`.

[`tools/benchmark_identity_digests.py`](../../tools/benchmark_identity_digests.py) accepts those two compiled directories and a new output directory; its fixed cases/counts and receipt checks are in source. Use `--rounds 1` for each separately coordinated block. Recompute the archive's complete summary with `python3 reproduce_summary.py --output recomputed.json final/0 final/1 final/2 final/3 final/4 final/5`.

The [machine-readable report](identity-hex-2026-11-23.json) contains full summary distributions and source manifests. The [retained evidence archive](identity-hex-2026-11-23.tar.gz) is **626,652 bytes**, SHA-256 **`1b9f271eaa685437ed1d1a68a2131b3493613f075906286fc612722300812527`**, with **596 members** including a manifest of every other member's length and SHA-256. It contains all raw final/pilot/diagnostic logs, exact final class trees, complete changed-class inputs, pinned libcore source, mutations, build/policy logs and integration receipts. Large shared dependency jars are identified by hash instead of redistributed.

This artifact proposes a focused efficiency improvement with strong steady target reductions and preserved identity contracts. Current full fork CI, actual Windows/ART execution, cache-control acceptance and startup/tail budgets remain pending; neither #56 T12 nor #178 is closed by this component result.
