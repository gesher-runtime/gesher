# Inotify local-write routing — 2026-11-22

## Scope and result

This implements the identity-indexed local-write publication leaf of N52/T12 in
[issue #56](https://github.com/gesher-runtime/gesher/issues/56). The baseline is
`452372561e0a50c63f072a5ae1e7ca5aa81354b8`. It does not close the full inotify row.

Before this change, every successful local write with a live inotify instance
resolved a pathname into a 32,768-character temporary and scanned all watches.
The scan tested the parent pathname before inode identity, so an inode watch
missed writes through a hard link in a different directory. The change routes
file notifications by the actual writing handle's volume/file identity and
looks up directory watches by the writing handle's parent. It preserves the
application's file, write result, watch mask and event layout.

The whole production implementation and actual successful-write entry point
pass the host contracts; the shared public corpus passes against real Linux.
For 4,096 publications with 1,024 file watches, unmatched publications allocate
**12,288 → 0** objects and matched publications allocate **16,384 → 4,096**.
Both avoid all 4,096 pathname resolutions and 4,194,304 pathname comparisons.
These are component resource measurements, not Windows or whole-application
write-throughput measurements. The setup and memory costs are reported below.

## Contract and scope evidence

The primary sources are pinned to Android common kernel
`android16-6.12-2025-06_r38`:

- [include/linux/fsnotify.h](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/include/linux/fsnotify.h),
  SHA-256 `1d4f23c7e93d6b57c715c10c26431fbaf87f27ea31946ba2760f7193f56bfb5a`:
  `fsnotify_modify` reaches the file inode notification and the parent of the
  actual writing dentry. An inode watch is independent of another hard link's
  spelling; a directory watch observes its own child name.
- [fs/notify/inotify/inotify_fsnotify.c](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/notify/inotify/inotify_fsnotify.c),
  SHA-256 `3dc836dc11ebd4608b25275248d25ad7ae754601b6b75a522ea58b2ff50d35b5`:
  inode and child-name events retain their requested masks, and a one-shot
  watch retires after the accepted event.

No kernel source was copied into the implementation. The patch contains one
production file, `compat/posix/src/inotify.cc`. The existing successful-write
call sites in `fd.cc::TransferFileHandle` and the positioned-I/O path in
`unistd.cc` already retain the actual native handle through publication.
Zero-byte and failed writes still publish nothing. No synthetic event is
substituted for a failed native operation.

The scope excludes external-writer relocation across directories, additional
OPEN/ACCESS/CLOSE_NOWRITE support, changes to the existing close-write probes,
and generic descriptor/epoll ownership. The separate #293 readonly-edge change
touches queue publication and is checked in a separate composition receipt.
The audited #73, #256, #268, #296 and #297 scopes do not own this routing path.

## Ownership, ordering and error paths

Each inotify instance indexes active watches by volume/file identity and keeps
ordered parent-name ranges for directories and the existing unknown-ID file
fallback. The index owns no additional watch or native handle. All admission,
lookup and removal use the existing source mutex; the existing global-instance
lock precedes that mutex. A watch is indexed only after native read admission
succeeds. The first `End` or worker shutdown transition unindexes it before
canceling its native read. Later canceled completions cannot erase a successor
watch with the same file identity because removal also checks the descriptor.

Known-ID file watches need at most one actual handle-identity query across all
instances, without a pathname. Directory or unknown-ID watches resolve the
writing pathname once, then visit only the matching ordered ranges. The code
merges those ranges with the inode match in descriptor order without per-write
scratch allocation. Iterators advance before publication, because an accepted
one-shot event can erase the current index entry. Duplicate add, MASK_CREATE,
MASK_ADD and exact mask replacement retain the existing descriptor and rules.

The actual native-record drain still precedes a local file modification. Its
64-completion bound, overflow behavior, read-rearm failures, native rename and
removal handling, event coalescing, wake publication and write-close tracking
are unchanged. An identity-query failure suppresses an unverified file match
while valid directory events can still use the writing pathname. A pathname
failure no longer hides a verified inode event. Volumes with unusable file IDs
retain the previous name fallback and duplicate-admission behavior; this leaf
does not establish broader zero-ID filesystem compatibility.

## Measured resource work and costs

The same GCC 14.2 `-O1` build, without sanitizers, compiles the whole unchanged
baseline or candidate source into the same declared host boundary. Eight fresh
processes ran in **ABBA ABBA** order (A baseline, B candidate). Every measured
source and binary hash, raw run, log hash and median is retained in the
[JSON receipt](inotify-local-write-routing-2026-11-22.json).

The measured loop calls the actual `InotifyNoteWrite(HANDLE)` on a retained
handle backed by a real Linux file. The loop measures publication only: it
does not include the write syscall or the Windows completion worker. The
functional corpus separately executes the actual `TransferFileHandle` and
real Linux `write`. Allocation counters include C++ allocations in production
and the same declared adapters. Path and identity queries use real Linux
readlink/fstat. The host has four-byte `wchar_t`, so its allocated byte counts
are **not Windows byte counts or RSS**.

| Workload, 4,096 publications | Baseline | Candidate |
| --- | ---: | ---: |
| 1,024 file watches, unmatched allocations | 12,288 | 0 |
| Unmatched allocated bytes on this host | 537,755,648 | 0 |
| 1,024 file watches, matched allocations | 16,384 | 4,096 |
| Matched allocated bytes on this host | 537,825,280 | 69,632 |
| File workload pathname resolutions | 4,096 | 0 |
| File workload pathname comparisons | 4,194,304 | 0 |
| File workload identity queries | 4,096 | 4,096 |
| Matched wake publications | 4,096 | 4,096 |
| 64 directory watches, unmatched comparisons | 262,144 | 40,960 |
| 64 directory watches, matched comparisons | 262,144 | 45,056 |

The retained matched-path allocation constructs the ordinary 17-byte event
string before duplicate coalescing. It is not eliminated. Directory workloads
still require 4,096 pathname resolutions and retain the same allocated bytes
and objects: 12,288 unmatched or 20,480 matched. The existing global walk of
inotify instances and worker close-write probes remain unchanged.

| Observed median publication time | Baseline | Candidate |
| --- | ---: | ---: |
| One file watch, 4,096 unmatched | 12.040 ms | 0.664 ms |
| One file watch, 4,096 warm matched | 12.592 ms | 0.838 ms |
| 64 file watches, 4,096 unmatched | 82.273 ms | 0.692 ms |
| 64 file watches, 4,096 warm matched | 83.512 ms | 0.886 ms |
| 1,024 file watches, one cold match | 492.012 µs | 2.995 µs |
| 1,024 file watches, 4,096 unmatched | 1,202.935 ms | 0.712 ms |
| 1,024 file watches, 4,096 warm matched | 1,251.925 ms | 1.198 ms |
| 64 directory watches, 4,096 unmatched | 81.167 ms | 21.832 ms |
| 64 directory watches, 4,096 matched | 93.254 ms | 24.555 ms |

These wall times were observed on a shared executor and depend on the declared
host boundaries. The enforced benefit is removal of the pathname temporary
and full watch scan, not a promised timing ratio on Windows.

There is a real setup cost. On this host, `sizeof(Inotify)` increases from
256 to 408 bytes; `sizeof(Watch)` stays 272 bytes. Each known-ID file watch
adds one 56-byte map allocation. For 1,024 watches, measured setup allocations
rise from 19,456 to 20,480 and allocated bytes from 68,907,344 to 68,964,688.
The 64-directory setup adds 192 allocations and 21,720 bytes because it also
owns ordered path keys; that cost depends on path length.

Observed 1,024-file setup time increases from 48.111 to 77.929 ms (about 62%).
One-watch setup increases from 38.113 to 83.922 µs; 64-file setup is about
3.36 ms in both. Directory setup timings vary strongly and do not support a
setup-speedup claim. This trade favors repeated local writes with installed
watches; it does not make every operation faster.

## Validation and maintained gates

- `tools/test_inotify_records.py --sanitize` retains its original **10** native
  record assertions and invokes the new helper. Final execution passes the
  shared **62** real-Linux checks and **244** whole-production host assertions
  under ASan/UBSan. Only local leak detection is disabled because this executor
  uses ptrace, which prevents LeakSanitizer from running; CI flags are unchanged.
- The public corpus covers cross-directory hard links, duplicate add and mask
  updates, one-shot retirement/re-add, rename and replacement while a file stays
  open, correct writing-parent names, independent observers, and zero/failed
  writes. The same corpus is compiled into the actual Windows `posix_probe`;
  track 21 requires its completion marker and the existing all-passed marker.
- Additional host controls exercise both native record layouts, pending rename
  before local modification, canceled completion versus successor identity,
  read rearm failure, the 64-read loss boundary, known-ID path failure,
  unknown-ID fallback, descriptor ordering with simultaneous one-shot removal,
  masks and balanced native resource retirement. The final unlink test uses a
  real unlinked-but-open file and an explicit native-completion adapter to
  check the existing `Complete`/`End` retirement path. It does not establish
  Linux/Windows equality for the timing of DELETE_SELF on an open inode.
- The unchanged baseline runs the same common functional corpus and reports
  **139 checks / 11 expected failures**, specifically the hard-link and
  verified-inode-without-path behaviors repaired here. Candidate-only index
  invariants account for the different total; baseline is not reported as
  having run all 244 candidate assertions.
- Full changed `inotify.cc` and full `posix_probe.cc` compile with warnings as
  errors for Windows x86_64 and aarch64 using the verified LLVM-MinGW
  20261006 publisher toolchain. The actual PowerShell parser accepts track 21.
- The unchanged selected-source preservation selftest passes all **20**
  negative controls. No workflow pin, authority predicate, deadline or
  canonical receipt rule changes in this leaf.
- Independent review inspected actual production and the pinned source,
  replayed **62 Linux + 236 production** assertions under ASan/UBSan, and
  inspected the final eight unlink/retirement assertions. It found no
  actionable scope, lock, iterator or ownership blocker. The final commit
  binding is supplied in the publication review.

The whole source is compiled, but Win32 IOCP delivery, threadpool waits,
events and the virtual inotify registry remain explicit host adapters. The
host executes real admission, routing, draining, record completion and reads;
it does not run the production Windows completion worker. UTF-16/native record
ABI, real Windows filesystem behavior, Windows/ART runtime and full CI were
not executed locally. Cross compilation and target-gate wiring do not claim
those results.

## Reproduction

```sh
# Maintained gate, including the existing native-record assertions.
python3 tools/test_inotify_records.py --sanitize

# Whole-source functional oracle/control; historical source is expected to fail.
python3 tools/test_inotify_write_routing.py --source-ref \
  452372561e0a50c63f072a5ae1e7ca5aa81354b8

# Work-only builds. Run retained production-host --measure binaries in ABBA order.
python3 tools/test_inotify_write_routing.py --measure --keep-dir /tmp/routing-candidate
python3 tools/test_inotify_write_routing.py --measure --keep-dir /tmp/routing-baseline \
  --source-ref 452372561e0a50c63f072a5ae1e7ca5aa81354b8

python3 tools/net-tcp-priority/test-selected-policy.py --selftest
```

## Separate #293 composition

The reviewed standalone implementation is commit
`914df21f5675b5abb60ad53be5151ec49bbd8730`, tree
`66a3875c4065e5e6e2e28f8f5fbf624e67cc5a7a`. An isolated worktree merged
#293's local `6c97f416d9061c20ae5758fc4bcfed508bc06e5f` (the exact published
tree `e4c4a4e3a55a5a10b4db32ab155a7d4d9a7d8c58`) without manual conflicts.
The resulting commit is `8e6562a53ba7e9b5f94f47c328e332e121c82138`, tree
`55bf8a55f0dab47d7623d8084567383e2be95f0f`.

The only shared file is `inotify.cc`: its active identity/parent indices
coexist with #293's single-owned-overflow marker and read-edge publication.
Its combined SHA-256 is
`fc72941a3d264ac1fcb0fdefd394149b423e6bc99067c6d09ef47af0540148c3`.
All 12 disjoint #293 files and all eight disjoint files from this change are
byte-identical to their incoming versions; every other existing Git leaf
(4,352) remains unchanged.

On this exact combined source, the maintained inotify gate passes **10 native
record + 62 Linux + 244 production** checks, and the readonly-edge gate passes
**137 Linux + 535 production** checks under ASan/UBSan. Its work controls still
show 10,000 unread waits without repeated timer/inotify delivery and 10,000
periodic ticks accounted for with one callback/arm. The whole combined
`inotify.cc` also cross-compiles for both Windows ABIs with warnings as errors,
and the original 20 preservation negatives pass. Commands, source identity,
preserved paths and log hashes are recorded in the JSON receipt.

This is separate integration evidence. The earlier standalone ABBA resource
measurements retain their original source identity; they are not relabeled
as measurements of this combined tree. No combined Windows/ART runtime or
full-CI pass is inferred from the host and cross-compilation checks.
