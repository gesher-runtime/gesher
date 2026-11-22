# Bounded Bionic binary substring search

Baseline: `751d82310f7c4c470c098ccebfd5d2d04699a0cd`. Refs #56 N16/T12; partial native efficiency evidence for #178.

## Production change and compatibility

The exported `memmem` previously called a full `memcmp` at every possible start. Repeated-prefix binary input could therefore require work proportional to haystack length multiplied by needle length. The replacement retains the existing Bionic export and guest calling convention, using an independently written Two-Way search for long needles and optimized byte scans for short needles. It has constant temporary storage and makes no heap allocations.

The critical factorization and periodic-prefix memory follow the algorithm described by [Crochemore and Perrin, *Two-Way String-Matching*, JACM 38(3), 651–675 (1991)](https://monge.univ-mlv.fr/~mac/Articles-PDF/CP-1991-jacm.pdf). No third-party implementation source was copied. New code follows the project Apache-2.0 license.

First-match selection, arbitrary binary bytes, embedded NULs, unaligned input, empty needles and errno are preserved. All bounds use remaining lengths, and guarded start/end fixtures exercise both inputs. Tiny needles compare at most 16 bytes per candidate; a near-whole-buffer input has at most four remaining candidates. Long searches retain only scalar state. Immediate matches skip factorization.

## Measured work and CPU

The fixed 16,384-byte haystack / 1,024-byte repeated-prefix miss performs **15,729,664 → 19,455 compared byte pairs**, a **99.876% reduction**. The instrumentation counts actual production comparison sites and host memchr/memcmp byte comparisons in a separate copy; the CPU measurements below execute uninstrumented code. The maximum emitted production frame is **80 bytes** with GCC 14.2 `-O2` in this host fixture.

Five A/B pairs alternate execution order. The baseline wrapper is verified byte-for-byte against the pinned Git commit, and baseline/candidate compile in separate translation units with identical flags. Process CPU time excludes input generation and uses reused warm input on a shared Linux x86-64 host. These are component measurements, not application startup, Windows latency or total CPU savings.

| Workload | Haystack / needle bytes | Baseline median ns/call | Candidate median ns/call | Baseline / candidate |
| --- | ---: | ---: | ---: | ---: |
| tiny-end-match | 32 / 2 | 93.510 | 8.745 | 10.69× |
| tiny-immediate-match | 32 / 2 | 4.150 | 6.170 | 0.67× |
| single-byte-end-match | 32 / 1 | 97.650 | 4.145 | 23.56× |
| empty-needle | 32 / 0 | 1.775 | 2.800 | 0.63× |
| tiny-miss | 32 / 8 | 66.870 | 5.310 | 12.59× |
| short-random-miss | 4,096 / 8 | 11,500.250 | 122.750 | 93.69× |
| long-random-miss | 1,048,576 / 64 | 3,014,125.000 | 926,375.000 | 3.25× |
| middle-match | 1,048,576 / 64 | 1,578,750.000 | 456,625.000 | 3.46× |
| repeated-prefix-miss | 1,048,576 / 4,096 | 52,916,500.000 | 888,750.000 | 59.54× |
| periodic-miss | 1,048,576 / 257 | 3,826,125.000 | 917,000.000 | 4.17× |
| immediate-long-match | 1,048,576 / 4,096 | 52.730 | 51.760 | 1.02× |
| whole-buffer-miss | 4,096 / 4,096 | 5.620 | 5.160 | 1.09× |

**Trivial-call tradeoff:** empty-needle and immediate two-byte matches do not benefit from scanning improvements; this run measures approximately 1 ns and 2 ns more CPU per call respectively. Their percentage differences are large at this scale, and no speedup is claimed for them. Immediate long matches and whole-buffer misses are effectively unchanged. The nontrivial scans improve substantially; realistic application frequency and Windows measurements remain necessary before claiming a product-level benefit. No generic timing threshold is used as a correctness gate.

Full repetitions, source SHA-256 and environment are in [`bionic-memmem-2026-11-22.json`](bionic-memmem-2026-11-22.json).

## Validation and reproduction

- **426,273 contract/work checks** pass normally and with ASan/UBSan: 120,000 seeded binary inputs, exhaustive expanded binary words, guard pages before/after each operand, direct comparison with an independent naive oracle, and deterministic linear-work limits. The benchmark adds 12 parity checks.
- The shared C fixture adds 50 checks to the existing imported-libc tests, including 4,096 randomized cases and all 256 one-byte values. The Windows x86-64 and translated ARM64 native suite will exercise the actual exported entry point. Those target runs were not executed locally.
- The normal CI command uses a frozen eight-line prior implementation and does not need historical Git objects. `--compare-ref` verifies that control against the requested commit for A/B provenance.
- Local LeakSanitizer cannot inspect threads under this container's ptrace runtime. `ASAN_OPTIONS=detect_leaks=0` disables only that component; ASan, UBSan and guard pages remain active. CI keeps default sanitizer settings.

```sh
python3 tools/test_memmem_performance.py
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_memmem_performance.py --sanitize
python3 tools/test_memmem_performance.py --measure \
  --compare-ref 751d82310f7c4c470c098ccebfd5d2d04699a0cd
```

Windows compilation/execution, Android-reference runs, representative native application workloads, and the complete #56/#178 acceptance and soak criteria remain open. No app capabilities, permissions, inputs, workflows or public results are substituted.

## CI source-preservation qualification

The first published CI attempt reached the exact-source gate before native
compilation and rejected the changed `shim.cc` whole-file digest. The follow-up
keeps the existing OFD manifest and its pinned digest unchanged. It pins the new
search helper independently, requires exactly the reviewed include and complete
`Memmem` wrapper, reverses only those substitutions in memory, and checks the
original whole-file digest. Every unrelated shim byte remains protected.

`python3 tools/net-tcp-priority/test-selected-policy.py --selftest` passes with
all 20 negative controls, including five new controls for unrelated shim edits,
changed search arguments, missing/duplicate includes, and helper changes. The
existing supplier, socket, ownership, fixture, deadline, DAG and runtime gates
are unchanged. This source qualification establishes no native runtime approval
or Windows execution result.
