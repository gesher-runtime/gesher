# Independent futex buckets without a process-wide queue lock

## Scope and contract

This is the queue-lock implementation for NATIVE-07 in issue #178, based on
`8b316aebf09ab46d5407e955bf86ed748a1e739f`. It preserves the supported wait, wake,
bitset, private-key, requeue, deadline and executable-owned provider contracts.
The limitations documented in
[native traversal/futex contracts](native-traversal-futex-contracts-2026-11-13.md)
remain: this does not add PI futexes, robust owner death, cross-process aliases
or Linux signal interruption.

The previous 64-bucket queue used a single SRW lock. Every unrelated native or
Java synchronization address therefore contended with all other addresses.
Each bucket now owns a cache-aligned SRW lock. Requeue locks both participating
buckets in their static array order, taking one lock for collisions or a
same-address requeue. Stack waiters publish their current bucket atomically.
Timeout/return cleanup acquires that bucket and rechecks its identity before
reading links; it retries if a requeue moved the node. Wakers continue to signal
under the owning lock, so the waiter cannot return while a waker uses its stack.

Expected-value mismatch returns before initializing a waiter, and wake reuses
its already-computed source bucket. There is no new per-operation allocation,
thread, wait handle, cache or provider. The Windows x64/ARM64 bucket array is
4 KiB instead of 1 KiB plus one SRW lock; a live stack waiter gains one pointer.
Buckets with a common address or a hash collision still serialize deliberately.

## Measured contention and elapsed work

The host runner compiles the actual production `syscall.cc` from each revision,
including the ordinary syscall/provider entry. It substitutes the existing
Win32 transport adapter; its mutex acquisition first tries the lock so it can
count contention. This instrumentation is identical for both variants and is
absent from production. The tests below exercise empty wakes and immediate
expected-value mismatches, not blocking transport, ART or whole applications.

The recorded run used `c++ (Debian 14.2.0-19) 14.2.0`, `-std=c++20 -O2`,
250,000 calls per thread, ten samples per case, and alternating A/B then B/A
rounds. The source hashes are:

- Baseline: `d663f3a22c867f7644db872ff530443466e14f9ab95ea8f1e9bf16d94ebcf520`.
- Candidate: `50cec2cf7793dea5cf955527876ee4161df705a8230d7a3fce259dcf905f954a`.

Across independent-address cases, contended acquisitions fell from
**21,432,583 to zero** over **215,000,000 acquisitions per revision**.
All result/error checks passed. The zero-contention result is the central
measurement; elapsed ratios belong only to this host fixture.

| Threads, independent addresses | Wake baseline ms | Wake candidate ms | Wake ratio | Mismatch baseline ms | Mismatch candidate ms |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4.328 | 4.227 | 1.02× | 4.816 | 4.393 |
| 2 | 15.646 | 4.263 | 3.67× | 18.430 | 4.446 |
| 8 | 118.203 | 5.172 | 22.85× | 118.485 | 6.676 |
| 32 | 418.708 | 22.085 | 18.96× | 448.323 | 24.074 |

Medians for the same-address controls remain in the same range. These controls
are intentionally serialized; they do not claim a benefit from sharding.

| Threads, one address | Wake ms, baseline → candidate | Change | Mismatch ms, baseline → candidate | Change |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4.325 → 4.182 | -3.3% | 4.770 → 4.489 | -5.9% |
| 2 | 16.858 → 14.526 | -13.8% | 16.335 → 16.970 | +3.9% |
| 8 | 113.632 → 111.822 | -1.6% | 119.087 → 116.675 | -2.0% |
| 32 | 421.468 → 409.581 | -2.8% | 457.315 → 427.329 | -6.6% |

An initial development probe showed roughly 11% worse elapsed time in one
shared-address control. That concrete concern led to alternating longer runs
and removal of redundant wake hashing and mismatch-path waiter initialization.
The final implementation has no median increase above 4% in these controls;
raw samples still have substantial host-scheduling spread. This is not a
statistical Windows no-regression qualification or a release latency budget.
[Raw samples and metadata](futex-bucket-locks-2026-11-22.jsonl) retain every case,
including controls, process CPU observations, ranges reconstructible from the
samples, and the exact source identities. No favorable subset was discarded.

## Correctness validation

The shared C/C++ contract fixture now exercises opposing requeues, same-bucket
collisions, both key modes, bit masks, timeout during repeated migration and
wake/return while migration is active. Waiter admission uses observable requeue
results. A progress handshake precedes the concurrent-wake phase, avoiding a
scheduler-dependent assumption that a worker has already run. Both queues are
checked for retired stack nodes after completion.

- Production-source ASan/UBSan: public 81, mixed guest/host 166, concurrency 378
  assertions, all passing.
- Production-source ThreadSanitizer: mixed guest/host 166 and concurrency 378
  assertions, all passing.
- Real Linux syscall reference: 459 assertions, all passing.
- Clock-domain/deadline fixture: 74 assertions, all passing.
- Complete changed `syscall.cc` and `futex_test.cc` compile with the pinned
  LLVM-MinGW 20261006 for Windows x64 and ARM64, with production definitions,
  actual Windows headers, `-O2 -Wall -Wextra -Werror -fno-exceptions`.

The existing CI invocations already execute the expanded host, Linux-reference,
TSan and native/guest fixtures; no existing assertion or gate was weakened.
Local ASan disables only LeakSanitizer's unavailable ptrace process inspection.
Fresh Windows/ART execution and representative synchronization workloads remain
required before marking NATIVE-07 fully accepted. Cross-compilation is not a
Windows runtime result.

## Reproduction

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_futex_contracts.py --sanitize
CXX='c++ -fsanitize=thread -fno-omit-frame-pointer' \
  python3 tools/test_futex_contracts.py --group guest --group concurrency
python3 tools/test_futex_contracts.py --linux-reference
python3 tools/test_clock_domains.py
python3 tools/benchmark_futex_contention.py \
  --baseline-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f \
  --iterations 250000 --samples 10 --output futex-results.json
```

On Windows, `futex_test.exe --benchmark` runs the same public-entry workloads
using real SRW locks and records JSON lines; unavailable lock-instrumentation
counters are `null`. The default invocation still runs the complete contract
suite. Native CPU samples use `GetProcessTimes` directly, so this benchmark does
not depend on the separate pthread CPU-clock provider repair in PR #113.
