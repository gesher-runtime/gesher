# NATIVE-13: structured logging overhead

Owner: `palmoni5`. Epic: [#178](https://github.com/gesher-runtime/gesher/issues/178).
Baseline: `638e19b4a6c109ec8881ba90a94d64b1817fc095`.

## Contract and implementation

This change preserves the existing synchronous structured logging contract:
one named-mutex-protected open/size/append/close operation per enabled record;
rollback after short or failed writes; the 16 MiB rotation bound; unchanged
binary records; and immediate visibility to readers and exiting processes.
There is no asynchronous queue, batching, suppressed log, retained data handle,
or new disk-durability promise.

The liblog priority rules follow the pinned Android implementation at
[`system/logging/liblog/properties.cpp`](https://android.googlesource.com/platform/system/logging/+/android-16.0.0_r4/liblog/properties.cpp):
the existing persistent/transient tag pair, global fallback, supported letters
and `false`, and the minimum-priority/default interaction remain unchanged.
The comparison oracle is separate from the production implementation. Explicit
lengths, null tags and embedded NUL bytes are tested without treating the tag
as a terminated string.

The process-wide priority cache has 64 fixed slots, each retaining at most 247
tag bytes; a compile-time assertion caps the complete cache at 20 KiB. Hash
collisions replace a slot and require an exact length/content comparison.
Longer tags retain the old uncached lookup. Cache keys contain both mapped-area
identity and its published generation. `GetAll` returns a revision from the
same snapshot as the values, avoiding a writer/device-switch race between the
initial revision check and the lookup. No extra cross-process property mutex
is taken on cache hits. Minimum priority and caller fallback are never cached.

The mapped-area fast path still checks `GESHER_BINDER`, the standalone session
when applicable, and both inherited-root environment values. A mismatch uses
the existing area selection/root repair. `ANDROID_DATA` changes within an
existing boot retain the original boot root, as before.

Each record uses an 8232-byte automatic byte array rather than a heap string.
Common absolute log destinations use an allocation-free cached path check;
relative/long destinations retain the existing resolution behavior. The
additional retained cache is per process, not per thread. No file handle or
worker is added.

## Local measurements: explicit host boundary

`native13-host-samples.csv` contains all five repetitions for each revision
and enabled/disabled scenario, taken on the same Linux executor with GCC 14.2,
C++20 and `-O2`. The fixture compiles each revision's complete, unedited
`logging.cc` and matching public headers. It declares the Win32 file/mutex and
property storage boundary; it does **not** run Windows I/O or the production
mapped property area in this mode. File API boundary allocations are excluded
from C++ production allocation counts. Timing includes those boundary calls.

| Scenario | Calls per repetition | Baseline allocations/call | Candidate allocations/call | Baseline allocated bytes/call | Candidate allocated bytes/call |
| --- | ---: | ---: | ---: | ---: | ---: |
| Repeated disabled tag | 200,000 | 3 | 0 | 159 | 0 |
| Enabled tag + 192-byte text | 2,000 | 5 | 0 | 552 | 0 |

Raw CPU and wall times are retained in the CSV. These short host measurements
are not Windows speed ratios, whole-application CPU/RAM improvements, p95
latency evidence or representative production workload qualification. Memory
allocation bytes are traffic, not retained/private commit. The Windows fixture
prints actual process private commit and working set separately.

One open, one size query, one write and one close per enabled record remain;
disabled calls issue none. **Kernel file I/O operations were not reduced.**
Keeping those operations preserves deletion/recreation and synchronous
visibility without introducing a new batching contract. Thus this implements
the allocation/cache portion of NATIVE-13, not its full epic acceptance.

Reproduce the same baseline and candidate fixture:

```sh
python3 tools/test_structured_logging.py --source-ref 638e19b4a6c109ec8881ba90a94d64b1817fc095 --measure-only
python3 tools/test_structured_logging.py
python3 tools/test_structured_logging.py --sanitize
```

The script prints SHA-256 identities for every production source/header it
compiles. The baseline has nonzero allocation counts and is run with
`--measure-only`; its semantic tests still run. Current-source gates require
zero warm ordinary-tag/destination allocations and no additional property
mutex waits. Latency is reported rather than used as a noisy ratio gate.

## Windows and remaining acceptance

`--compile-only --cxx <llvm-mingw-clang++> --output <test.exe>` builds the
Windows fixture with the complete actual `system_properties.cc` and logger.
Only API-counting and one-shot short/failed-write wrappers are injected; all
Windows API operations delegate to the real kernel. The fixture checks real
cross-process generation changes, equal-generation area switches, inherited
root repair, concurrent priority updates, exact records, partial append
rollback, deletion/recreation/rotation and a child that records then exits
immediately without destructors. It measures both enabled and disabled paths.
The required native shard runs it on Windows 2022 and 2025.

Existing mandatory `exec3_property_host_probe` and `Exec3LogTest` continue to
cover an idle *other process* during delete/recreate, cross-process clear,
16 MiB rollover, extended paths, Java/NDK loggability and actual logcat formats.

Local cross-compilation is not a Windows runtime result. Actual Windows gate
results must be attached to the PR for its reviewed head. Representative real
logging load, many-tag churn, multi-application contention, statistically
useful tail latency/private commit comparisons and the epic's eight-hour soak
remain outside the local host evidence. Do not mark NATIVE-13 or #178 complete
on the strength of these allocation numbers.

## Composition with the shared process environment

After main merged #118, CI run
[45026786455](https://github.com/gesher-runtime/gesher/actions/runs/45026786455)
tested merge `e26b7de54f7437b1f2a9fb7d21a29c8ea134dca7` (tree
`633fbd46f06c11c42891ed8847d8670b085f782a`). Its parents were main
`1a3a5c244d643c7c93b621326857c6b8c858d5bb` and logging head
`e1f55514e7f341583f72710fba9ae49fa9235a92`. Two integration problems appeared:
the new OFD source receipt pinned main's property file before the logging
revision additions, and the standalone Windows fixture did not yet stage the
property file's newly required environment owner.

The source qualification now reverses the exact NATIVE-13 property transition
before checking the **unchanged** OFD receipt. The composed file is pinned at
SHA-256 `fc2e4d97ee02d1f32e49aeccd648ee386cd44debf9092b4eef48b0c7e2f1499a`;
reversing `qualified/native13-property-source.patch` must recover the original
receipt's `a7891115d788a0a89745d389325cbe90a0b3b8c940651b2b3940f27693ac9989`.
The public revision header is separately pinned. The patch covers only
`system_properties.cc`: #118's environment include and all three
`SetWideEnvironment` calls remain in the recovered main file. No other OFD
receipt hash, supplier input, controller pin, native polling byte, fixture
deadline or workflow is changed by this repair relative to the failed merge.
`native_runtime_approval` remains false.

Six added negative mutations cover generation ordering, snapshot coherence,
device identity, the shared root setter, the public revision type and the exact
transition patch. Four property mutations also run with the new file hash
deliberately refreshed: the original receipt or reverse transition must still
reject them. The ordinary `test-vector-policy.py` CI entry point runs these
controls alongside its existing vector, descriptor, priority, census and receipt
controls. A changed hash alone does not qualify changed behavior.

When the selected property source requires it, the Windows fixture now compiles
the complete production environment owner, table and font writer, with the
same per-source exception setting as `posix.cmake`. It copies the complete
native environment-write helpers from `unistd.cc` and CRT error mapper from
`errno.cc` at explicit, checked boundaries. These helpers execute their actual
UCRT/Win32 calls; no replacement setter or successful-operation stub is used.
The runner prints both complete-source and extracted-region hashes. Historical
`--source-ref` builds stage only the dependencies used by that exact revision.

Additional Windows assertions compare the shared POSIX owner's root and session
with the actual Win32 environment after initialization and inherited-root repair.
The existing child must still publish a distinct value into the parent's
original mapped area. Local cross-compilation verifies both the composed fixture
and the original `638e19b4` baseline; fresh Windows 2022/2025 execution of the
composed fixture remains required.

Focused repair checks:

```sh
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
python3 tools/net-tcp-priority/test-vector-policy.py
python3 tools/net-tcp-priority/test-selected-supplier.py
python3 tools/host-pthread-authority/minimum_receipt.py selftest
python3 tools/test_environment_contracts.py
python3 tools/test_structured_logging.py
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_structured_logging.py --sanitize
python3 tools/test_structured_logging.py --compile-only --cxx <llvm-mingw-clang++> --output out/structured_logging_test.exe
python3 tools/test_structured_logging.py --source-ref 638e19b4a6c109ec8881ba90a94d64b1817fc095 --compile-only --cxx <llvm-mingw-clang++> --output out/structured_logging_baseline_test.exe
```

Leak detection was disabled only for the local logging sanitizer invocation;
the CI command is unchanged. These checks qualify the source composition and
test build, not a new performance result or a native runtime approval.
