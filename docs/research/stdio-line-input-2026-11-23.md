# Buffered native and Bionic line input — 23 November 2026

## Scope and Android contract

This is the N40/T12 line-input leaf of #56. The baseline is upstream
`6afcb6e671fc4f2d08a4a98220f9d9a906a327bf`. The implementation changes shared stdio input, native and
Bionic `getdelim`/`getline`, and bounded `fgets` input. It does not change
descriptor identity, permission checks, read-ahead capacity, I/O callbacks,
blocking mode or stream-lock ordering.

The pinned Android 16 reference is
[getdelim.c](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/upstream-openbsd/lib/libc/stdio/getdelim.c)
(SHA-256 `8d70861a1012e67ac418db31dab3eae92912c163e84fe85a1a7ec48f209b9c3f`)
and [stdio.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/stdio/stdio.cpp).
The implementation is original Gesher code. Those references establish the
observable rules: include the delimiter, append a separate NUL, retain
read-ahead and pushback, set the error indicator on invalid output/growth,
reject partial input errors from `getdelim`, preserve a partial `fgets` result,
and check the signed result plus terminator before allocation. Delimiter
matching follows Bionic's unsigned-byte conversion, including NUL.

Previously each byte read by native/guest `getdelim` and guest unlocked
`fgets` acquired the shared stream registry and an owning reference. The outer
call already held the stream lock. Native `fgets` kept one owner but still
processed every byte through the single-byte read state machine. The new
shared implementation searches existing buffered spans and copies them once.
Pushed-back bytes retain their reverse order and are not consumed until growth
succeeds. `getdelim` growth uses Bionic's power-of-two capacity and zeroed new
tail. The versioned `gesher_stdio_delimited_v1` entry shares the existing owner
without extending the v1 function-table layout.

## Before/after workload

Each sample reads 64 records of 65,536 bytes, or **4,194,304 bytes**. Every
returned record is compared in full with the source, including length and
terminator. The Linux filesystem, descriptors and production core, adapter,
native entry, and guest entry bodies execute. Win32 owner discovery, FILE
allocation/locking and Bionic metadata orientation are declared host boundaries.
`registry_visits` counts the existing owner-initialization boundary reached by
production registry lookups. It adds no counter to production code.

The sequence was baseline, candidate, candidate, baseline, five samples per
operation per execution. Raw outputs and source hashes are in
[stdio-line-input-2026-11-23.json](stdio-line-input-2026-11-23.json).

| Entry | Registry visits before → after, per 4 MiB | Median host time before → after |
|---|---:|---:|
| Native `getdelim` | 4,194,368 → 64 | 132.1745 → 0.7355 ms |
| Bionic `getdelim` | 4,194,368 → 128 | 135.6920 → 0.6620 ms |
| Native `fgets` | 64 → 64 | 25.0900 → 0.6465 ms |
| Bionic `fgets_unlocked` | 4,194,368 → 128 | 133.0945 → 0.6685 ms |

Cold `getdelim` reallocations fall from **11 to 5**; warmed caller buffers need
none in either revision. These are component results under the stated host
adapter, not Windows timings, whole-application speedups or RSS measurements.
The repeated initialization boundary in this adapter is heavier than Windows'
completed INIT_ONCE fast path; the timings must not be extrapolated to Windows.
The owner-lookup reduction is independently apparent in the actual production
call paths.

There is no additional persistent stream storage. The old guest 120-byte
doubling policy could retain 122,880 bytes for this 65,536-byte record; the
Bionic-compatible power-of-two policy retains 131,072 bytes, as the native
path already did. This extra 8,192 bytes of caller-owned capacity in the
declared workload is disclosed rather than counted as a memory reduction.

## Validation

- Production buffering core: **306 assertions**. Includes short refill,
  `EINTR`, signed overflow before allocation, zeroed growth, retained prefix
  and unread next span after allocation failure, inline and overflow pushback.
- Complete shared adapter: **1,579 assertions**, including **1,435 new line
  assertions**, real files and nonblocking pipes, every byte delimiter,
  embedded NUL, capacity-one `fgets`, all buffering modes, EOF, partial
  `EAGAIN`, allocation failure, two simultaneous native/guest readers,
  cancellation and existing stream lifecycle/descriptor tests.
- Actual Bionic metadata/close/reopen envelope: **118 assertions**.
- Existing portable stdio/OFD Linux reference: **71 assertions**.
- The public Bionic stdio test's Linux reference passes **43 assertions**;
  the Windows/Android branch additionally checks Bionic error flags.
- The unchanged baseline fails **17 of the 1,435 line assertions**. These
  expose existing empty-EOF allocation/capacity, unsigned-delimiter,
  partial-error, invalid-argument and allocation-cursor bugs. Some downstream
  assertions fail because the wrong delimiter consumed a later record; the
  report does not count each as an independent defect.
- All three changed production translation units and the public Bionic test
  compile for Windows x64 and ARM64 with the repository's pinned
  `llvm-mingw-20261006-ucrt` toolchain.

ASan/UBSan pass locally with `ASAN_OPTIONS=detect_leaks=0`, because this
executor's ptrace restriction prevents LeakSanitizer's process inspection.
No sanitizer setting or existing runtime acceptance gate was relaxed. The expanded host corpus
runs in the existing `test_stdio_contracts.py` gate; the public Windows checks
run in the existing `bionic_stdio_contract_test` target. Real Windows UCRT,
ELF ABI and ART execution require the full fork CI and are not claimed by the
local results. No broad #56 requirement is marked complete.

### Source qualification

The selected TCP supplier gate pins stdio as part of its OFD dependency graph.
Its original source-policy run rejected this production change at the
`stdio_output.cc` digest. The additional `stdio-line-source.json` and patch
admit exactly the five changed production files, with independently fixed
manifest, patch, before and after hashes. The checker reverses that transition
to the complete `6afcb6e6` files, verifies their Git bytes and previous digests,
then forwards it again to reproduce the complete candidate files. All original
OFD preservation checks run on the restored bytes; the earlier OFD manifest,
patch, 71 source digests and `native_runtime_approval: false` remain unchanged.

`test-selected-policy.py --selftest` passes its original 20 mutation controls
and seven additional controls covering each admitted production file,
transition corruption and an attempted runtime-approval change. This is source
qualification only; it does not grant runtime acceptance or replace the
existing Windows tests.

## Composition with current main and shared stdio ownership

The current branch composes the previously published `4328a34d` with main
`8f53b6045e26f0bb4a482c69099d084db3e94963`. This includes PR #73's shared
Bionic conversion-state lifetime and PR #371's canonical diagnostic baseline.
The production reference is `fdf4861b`; #371 changes only its diagnostic test.
The measurements and original source receipts above remain historical evidence;
no new timing or Windows result is inferred from this composition.

The guest `getdelim` keeps its original shared line-reader validation, stream
error flag and conditional orientation, and additionally stops when the new
shared `StreamGuard` cannot acquire its owner. Restoring main's older argument
return would have lost the required stream-error bit; the actual merged
regression corpus rejected that draft. The new guard-admission control uses a
one-shot real allocation failure and verifies that the entry does not retry,
consume input, allocate the caller buffer or retain a CRT lock. Span algorithms,
native entry points and the separate delimited-owner export remain unchanged. Deleting
only the guard-failure return from the actual production source makes this
1,606-check adapter fail exactly the new no-retry/no-consumption assertion;
the negative exits with one assertion failure and no sanitizer fault.

All incoming owner-lifetime, unlocked-I/O and late-socket-shutdown checks remain.
The lifetime envelope now verifies both ordinary owner-managed status locking
and explicitly unlocked status entries. Its supplied owner boundary remains
separate from the actual shared-owner corpus. The integrated ASan/UBSan replay
passes 306 core, 1,606 adapter, 122 lifetime and 71 actual Linux reference checks,
plus the existing exit-order and exact socket-retirement negative controls.
Full production `stdio_output.cc`, Bionic stdio and the public native fixture
strictly compile for Windows x64 and ARM64. Actual Windows/ART/ELF execution
still requires full CI on the new published head.

The original line-input, PR73 and OFD manifests and patches remain byte-for-byte
unchanged. A separately pinned five-file current-main patch reconstructs the
complete `fdf4861b` source bytes before the original four-file PR73 transition
runs. The original line-input receipt is independently replayed against the
immutable published first-parent files, and its normalized sources must equal
the current-main/PR73 route. All 71 original OFD digests and explicit false
runtime approval remain. The integrated policy passes 34 transition controls,
four repinned-property controls, six repinned-current-stdio controls and all
25 shared-stdio controls. Supplier metadata retains all 36 semantic mutants
and its authority-hash negative; these checks grant no runtime approval.

The incoming published `92c87bef` framework reference and exact `6dc4cca5`
recipe remain intact. No CI limit, test assertion or supplier gate is removed.
The old full-fork run `45153439733` remains incomplete: its required native
blocking producer was canceled at the 35-minute prerequisite timeout after a
verified toolchain download. A fresh full run is required for this merge.

## Reproduction

For the current integrated correctness gates:

```sh
python3 tools/test_stdio_contracts.py --sanitize
python3 tools/test_stdio_lifetime_contracts.py --sanitize
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
```

For the original before/after measurement and baseline defect reproduction,
use the matching historical helper and fixture before PR73 changed ownership:

```sh
git worktree add /tmp/stdio-line-measured 4328a34d04e63f6f3881b34c7dca09ef1fa8ec0b
cd /tmp/stdio-line-measured
python3 tools/test_stdio_contracts.py --line-benchmark
python3 tools/test_stdio_contracts.py --line-benchmark --source-ref 6afcb6e6
# Negative control: the unchanged baseline is expected to exit nonzero.
python3 tools/test_stdio_contracts.py --line-only --source-ref 6afcb6e6
```

`--source-ref` reads the original adapter, both public entries and private
buffer headers directly from the named Git object; it does not substitute a
handwritten baseline.
