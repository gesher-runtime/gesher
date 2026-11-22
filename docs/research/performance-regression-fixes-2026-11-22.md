# Performance regression follow-up — 2026-11-22

This change follows a comparison of main at
`8b316aebf09ab46d5407e955bf86ed748a1e739f` with the approximate preceding-week
baseline `4f9d48d35817a7fa7b037b08afce090a874a3794`. The implementation is based
on `8b316aebf0`. The comparison identified additional work in particular paths;
it did not establish one application-wide slowdown percentage.

The goal is to remove repeated work while retaining Android-visible identity,
permission, stream, descriptor, rendering and lifecycle contracts. Required
verification and synchronization are not disabled to obtain a benchmark result.

## Disposition of the findings

| Audited path | Change or retained requirement | Contract preserved |
| --- | --- | --- |
| Normal permission checks scan every package and derive legacy signer keys | Boolean normal grants use manifest requests directly. A UID membership index follows the exact published package-map object. A verified APK generation hashes its signer certificate once. | Permission flags retain their complete path; shared UIDs, retirement, removal and replacement remain observable. No permission-result cache. |
| Startup repeatedly builds the permission catalogue for each UID | Collect declarations in one pass, take one boot catalogue snapshot, and reconcile only names requested by each admitted UID. Reuse the resolved definition. | Reserved platform names, conflicting signers, same-signer duplicates, runtime decisions and persisted flags retain their behavior. |
| Cold startup fully verifies installed APKs before UID admission | Retain full content verification for each newly parsed code generation. Reuse its verified identity within that generation. | Size, timestamps, a copied signing block or an earlier process's metadata cannot establish current signing identity or rotation lineage. |
| Later microG certificate policy reopens an already verified APK | Apply the official signer/scheme policy to that object's verified identity; retain the exact whole-file pin for the legacy v1 GSF artifact. | All current-file addon admission, override selection, packaging and pre-parse verification remain unchanged. |
| Per-character stdio takes a metadata-registry mutex | Eight positive thread-local entries refer to state owned by the existing FILE lifetime. An atomic bucket generation invalidates entries on retirement. | CRT FILE locking, stream orientation, multibyte state, pushback, conversion errors, close/reopen and allocation errors remain. |
| ANGLE serializes GL entry points to protect its shared D3D11 immediate context | Keep `angle_force_context_check_every_call`. Submit completion events through a small private ANGLE entry point under that same lock; wait outside it. | Different EGL contexts still synchronize use of the real shared D3D11 context. The locking correctness fix is not reverted. |
| Newly exposed EGL fences call `glFinish` during creation | Use real GPU completion events for asynchronous local fence creation and client status/waits. Native FD export waits for genuine completion outside ANGLE's lock. | No fabricated completed fence, arbitrary readable-FD import, cross-adapter dependency, or global GPU server wait is substituted for the requested synchronization. Native export's wait remains an explicit limitation. |
| ARM64 imported calls copy trampoline records under one process-wide mutex | Read coherent per-slot atomic snapshots; retry at most eight times before using the original mutex. | Bind, retirement and variadic-signature updates still serialize writers. Every copied field is atomic; records are not dynamically reclaimed. |
| Ordinary writes take the memfd registry mutex when any memfd exists | Use an 8 KiB atomic membership bitmap for FDs below 65,536, retaining the original high-FD registry. Ordinary closes avoid a read-modify-write when their bit is clear. | Actual memfd writes still read authoritative seal metadata. Duplication, import, close, reuse and independently changing bits are covered. |
| Unversioned launcher resources are SHA-256 hashed on both sides | Compare exact bytes in bounded chunks, stopping at the first mismatch. Keep version-generation invalidation and the ordinary stable-release fast path. | Equal-size/equal-time changes remain visible. Mapped identical resources are not rewritten. A failed copy is not acknowledged. |
| GPU keepalive gives the adapter work between sparse frames | Retain the existing default, adapter checks, opt-out and one-second idle stop. | The source documents an Intel sparse-frame readback problem that this mechanism addresses. Removing it without hardware evidence could restore those stalls. The thread already sleeps on a condition variable after idle. |
| Fixed-mode AppOps queries rebuild global UID state | Skip the process/activity/service snapshot when the result is independent of foreground state. | Foreground decisions and note/start history still refresh the required state; restrictions and caller validation remain. |

### Graphics scope and remaining wait

The new private ANGLE dispatch records a real DXGI completion event while
holding ANGLE's existing submission lock. Both KHR and native EGL fence creation
can return while the GPU is still working. Client waits, status queries and
native descriptor export do not hold that global submission lock or the EGL
sync registry lock while waiting. Pending events retain their device and event
ownership through completion, including after EGL sync destruction; a signaled
event caused by device removal is treated as an error, not successful rendering.

Native export remains deliberately narrower than a fully asynchronous Linux
sync-file transport: `eglDupNativeFenceFDANDROID` waits for genuine completion,
then publishes a typed, read-only completed event. It therefore still reduces
CPU/GPU overlap at the export boundary. Import accepts that descriptor type,
not an arbitrary readable file or pipe. Duplication, Binder and SCM_RIGHTS
preserve the type and keep transfer ownership until broker acknowledgement.
The Windows event's type/name is a compatibility identity, not a cryptographic
attestation against arbitrary native code in the same host environment.

A proposed pending cross-device D3D fence transport was rejected during QA:
queuing a foreign dependency on ANGLE's shared immediate queue could also block
unrelated EGL contexts. Local server waits instead use that queue's existing
ordering, and imported completed fences are already satisfied. There is no
cross-adapter pending dependency in the final implementation.

The required global ANGLE context serialization remains. Other application
`glFinish` calls and readbacks retain their existing synchronization; this change
does not remove every wait in ANGLE. An older installed SDK without the private
dispatch uses the genuine synchronous compatibility path. The new packaged
Windows test explicitly requires the dispatch so a stale SDK cannot silently
pass. The new shared descriptor holder adds no direct D3D/DXGI DLL imports to
the base POSIX library.

### Cold verification and conditional work

`PackageUids.prepare()` and the package-manager constructor do not perform two
full admission verifications of every ordinary APK. The second preparation sees
already admitted UIDs, and verified identity is cached by `ApkPackage`. The
metadata parser's compatibility certificate collection is not another full
apksig content verification.

Bundled microG has an additional, deliberate verification gate before parsing.
That gate and the standalone packaging preflight remain. Repeated identity
work in the later microG certificate policy is removed separately from general
APK admission; its duplicate layers predate the preceding-week comparison.
For the normal bundled GmsCore path this removes a third full verification
while keeping the pre-parse gate and the parsed generation's own verification.
Addon admission and override selection still recheck both the actual manifest
name and current signed contents. Independent QA demonstrated why replacing
those admission checks with cached identity would be unsafe, and that attempted
optimization was removed before publication.

The microG fallback's native-library byte comparison is conditional on the
enabled bundled fallback. It preserves mapped, unchanged libraries; ordinary
extraction previously rewrote those libraries. Usage history does not fsync on
every activity resume: its usual path updates memory and flushes periodically.
Day rollover, clock-generation correction, explicit purge and shutdown retain
their required persistence. These were assessed rather than classified as
unconditional regressions.

## Deterministic work-count results

The same permission runner was applied to a freshly built, unmodified
`8b316aebf0` application JAR and to the changed production classes. Host JNI
boundaries are explicit; this is not a Windows/ART application launch test.

| Fixture | Before | After |
| --- | ---: | ---: |
| 10,000 warm normal-permission checks; 20 installed packages, two shared-UID members: certificate hashes | 20,000 | 0 |
| Same checks: full package-map visits | 200,000 | 0 |
| 10,001 signer-fingerprint calls on one verified object | 10,001 hashes | 1 hash |
| Startup: 25 packages each declaring one custom normal permission | 16,900 package visits | 75 |
| Startup: 100 such packages | 1,020,100 visits in the preceding audit | 300 |
| Startup: 100 requested custom runtime definitions | — | 300 visits; all 100 complete UID states persisted |
| 1,000 fixed-mode AppOps checks | 1,000 global UID-state snapshots | 0 |
| First later microG policy check after full verification of the official 4,210,169-byte Companion APK | 4,199,678 additional SHA-256 input bytes | 753 certificate bytes |

The zero steady-state counts exclude initial index construction and mandatory
identity admission. The package-visit results concern these catalogue/index
paths, not every operation performed during boot. Saved-state trimming and full
APK verification remain.

The native metadata fixture also proves that 100,000 stable operations on each
optimized path finish while the corresponding writer/registry mutex is held.
They allocate nothing in those measured loops. Its timeout is a deadlock
watchdog, not a wall-clock performance budget.

## Informational timing

`tools/benchmark_stdio_metadata.py` extracts the production stdio bodies from
the selected immutable baseline and the current checkout. The fixture uses
Linux FILE adapters, buffered ordinary files, checked content and EOF, and
seven alternating before/after samples. A separate execution after integration
produced:

| Workload | Baseline median | Changed median |
| --- | ---: | ---: |
| One thread, 4 MiB character-by-character read | 82.020 ms | 52.282 ms |
| Four threads, independent 4 MiB streams | 85.292 ms | 55.895 ms |

These observations reduce this isolated metadata cost by about 36% and 34%.
They neither establish a Windows speedup nor restore every cost relative to
the earlier-week implementation, which lacked some required stream semantics.
No noisy wall-clock threshold is added to CI.

A separate launcher probe compared identical 64 MiB files in seven alternating
fresh PowerShell 7 processes on Linux. Its medians were 136.9955 ms for the old
two-hash comparison and 109.3689 ms for the changed comparison. This includes
first-use comparator initialization but excludes process startup and file
fixture preparation. It is an informational host observation. Identical files
still require reading both contents; a first-chunk mismatch now stops after
512 KiB total rather than reading and hashing 128 MiB. Stable valid release
metadata already avoids this comparison and continues doing so.

## Memory, invalidation and concurrency

- Permission indexing retains references to the current published package
  generation. Publication drops old indexes; retirement is checked live.
- Signer fingerprints belong to one fully verified `ApkPackage`. Reinstalling
  or replacing a package creates a new object and cannot inherit that identity.
- Stdio uses approximately 192 bytes of thread-local cache per thread. Positive
  entries hold no FILE ownership. Reset increments the bucket epoch before
  erasure while the FILE ownership lock remains held. Epoch exhaustion disables
  caching instead of wrapping to an old identity.
- Atomic trampoline slots add approximately 512 KiB to the bounded slot table.
  Sequentially consistent version and field accesses prevent mixed signatures.
  Exhausted versions and prolonged concurrent mutation use the locked path.
- The memfd bitmap uses 8 KiB. It records descriptor type, never the current
  seals or an authorization result. CRT `fclose`/`freopen` now retire its
  membership after actual closure/replacement. The preexisting missing cleanup
  could leave stale classification and cause extra EA queries after reuse;
  it did not make unrelated files inherit seals. A failed operation that leaves
  the descriptor live, and null-path reopen, retain the membership.
- Typed native-fence metadata uses a bounded 8,192-slot shared-owner table
  (approximately 128 KiB) and a 1 KiB atomic membership filter. The registry is
  initialized lazily. Each outstanding GPU completion also owns its event/device
  until completion or removal; local fence creation does not allocate an
  exported file descriptor.
- Launcher comparison uses at most two 256 KiB buffers; small files allocate
  their actual length. Both streams retain read sharing and are disposed on
  success, mismatch and error.

## Validation and independent review

The server/launcher/native review was performed independently of their
implementation against local production review head
`f201a445233bf01eb5b6a879199fb0e85ab01ee5`. The reviewer inspected policy and the
actual diff, rebuilt the application, inspected callers, and found no unresolved
blocking defect in that scope. It also found the unnecessary ordinary-close
bitmap write, which was fixed and retested. Its additional short-read/error
tests are included in the launcher suite.

| Validation | Local result |
| --- | --- |
| Independently rebuilt full application, javac and D8 | Pass |
| Permission work-count/publication contracts | 36 checks pass |
| AppOps/PermissionChecker/lifecycle contracts | 489 checks pass |
| UID isolation, each of spoof-disabled and spoof-enabled modes | 85 checks pass per mode |
| Interrupted UID admission, unpublished and published recovery | Pass |
| Real generated APK signing and rotation fixtures | Pass |
| Official Companion/GSF identity, content mutation and addon admission | 31 checks pass in each spoof mode |
| Launcher suite, including independent short-read/error cases | 87 checks pass |
| Additional independent launcher binary/oracle probe | 206 checks pass |
| Native metadata fixture, ASan + UBSan | Pass |
| Native metadata fixture, ThreadSanitizer | Pass |
| Existing stdio lifetime and allocation failures, ASan + UBSan | 104 checks pass |
| Stream/device/native-fence/memfd retirement fixture and Linux reference | 272 and 36 checks pass |
| Exact production ARM64 rollback/CallSVC/slot reuse | Pass |
| Existing memfd backend/syscall/fault contracts | Pass |
| Complete changed stdio and memfd units, Windows cross-compilation | x86_64 and AArch64 pass |
| Complete translator unit with real pinned/patched Dynarmic | x86_64 Windows compilation passes |
| EGL production-body fixture, ASan + UBSan | 69 checks pass; the immutable baseline fails the three intended async/condition assertions |
| Binder production receive/transfer fixture, ASan + UBSan | 5,667 checks pass |
| Eventfd edge fixture and Linux reference | 152 and 116 checks pass |
| Epoll fairness, vector routing, select and poll scratch | 15, 74, 28 and 67 checks pass |
| Timerfd host corpus and Linux reference | 133 and 68 assertions pass |
| Nine changed/new graphics units, Windows object cross-compilation | x86_64 and AArch64 pass; changed units rechecked after final edits |
| Private ANGLE dispatch with the exact pinned upstream internal headers | Compiles; expected export directives present; GN insertion anchor uniquely verified |
| Workflow policy, release and cache contract suites | 11, 21 and 6 checks pass |

Local sanitizer executions disabled LeakSanitizer because of the execution
environment's ptrace limitation. ASan, UBSan and separate TSan execution remained
enabled. Windows object compilation is not Windows execution. The launcher
host run is PowerShell 7 on Linux; Windows PowerShell 5.1's .NET Framework branch
must run in the existing Windows jobs. Java host contracts do not replace ART
Binder execution. Physical-adapter readback/keepalive performance is not measured
by these probes.

The additional microG change was independently reviewed and rebuilt at local head
`f7640cc7bb75114c0ffd0d5d0ac44b74b4c1e8d8`. QA reproduced a real issue in the
intermediate object-only admission gate: metadata from official Companion could
be followed by actual bytes from the equally genuine GSF package. A second case
changed signed content after an earlier object verification while preserving the
manifest name. Both now fail at the unchanged production admission boundary,
and both regression cases are committed. The later generation-owned policy
optimization remains; no unresolved blocker was found in this scope.

The server/launcher/native reviewer also checked cumulative production head
`a15b7e96499b96949c56921026676ba647b96315`: the previously reviewed production
blobs are unchanged, the final CRT memfd cleanup preserves live-descriptor and
reopen semantics, and an independent repeat of its stream suite passed all
272 production-adapter and 36 Linux-reference checks.

Graphics received a separate independent review. The reviewer wrote fixture
code, not production graphics changes, and reviewed graphics head
`f73dbbc78874f2e382c1856998774394f8a1d16c` plus integration CI/packaging changes
`44688e52b7434e6df1b13441b69fc95aa0a3337e`. The corresponding cumulative local
integration head is `a15b7e96499b96949c56921026676ba647b96315`; the final production
memfd retirement change and its independent fixture are included there.

Resolved review findings include context isolation, fresh/imported fence
conditions, failed-import FD ownership, full-width attributes, calling-context
flush semantics, low-memory lifetime, concurrent destruction, device-loss
status, transfer retention, aliased Windows wait handles and CRT retirement.
The large-poll deduplication loop stops collecting after 65 distinct handles,
preserving the existing fallback without adding an unbounded quadratic scan.
All final local checks listed above passed. No unresolved static correctness
blocker was found in that scope.

The real `egl_fence_test.exe` checks actual ANGLE/DXGI completion, concurrent
contexts, pending destruction, lazy export, descriptor reuse and cross-process
handle adoption. It is wired into the existing Windows native track both with
the default device and with forced WARP. Those Windows runs and the complete
rebuilt ANGLE SDK link have **not been executed locally**. They remain required
CI evidence before a merge recommendation. Cross-compilation and host adapters
do not establish those results, physical-GPU performance, or application FPS.

The review SHAs above identify local review checkpoints. Publication may create
a consolidated commit through the authenticated GitHub account; its complete
Git tree must match the reviewed final local tree exactly.

### Reproduction

Use the repository's normal app build with its pinned JDK/framework/libcore and
apksig dependencies, then pass those outputs to:

```text
python3 tools/test_permission_performance.py --java-home JAVA_HOME \
  --framework-classes FRAMEWORK_CLASSES --core-classes CORE_CLASSES \
  --app-classes APP_CLASSES_JAR --apksig APKSIG_JAR
```

The same arguments apply to `tools/test_appops.py` and
`tools/test_package_uid_isolation.py`. The permission runner's `--case normal`,
`--case signer` and `--case startup` options accept the unmodified baseline JAR
as negative controls. The expected optimization-count assertions then fail.

```bash
python3 tools/test_native_metadata_hot_paths.py --sanitize
python3 tools/test_native_metadata_hot_paths.py --thread-sanitize
python3 tools/test_stdio_lifetime_contracts.py --sanitize
python3 tools/test_stream_device_lifetime.py --sanitize
python3 tools/test_arm64_pending_rollback.py --sanitize
python3 tools/test_memfd_contracts.py
python3 tools/test_egl_fence_contracts.py --sanitize
python3 tools/benchmark_stdio_metadata.py
pwsh -NoLogo -NoProfile -File tests/windows/shell/RuntimeFileChecks.ps1
```

### Concurrent PR compatibility

An independent QA-only union with PR #227 at
`68ebdce8f3895b670a65399dd978e7ae66298bfa` built cleanly and passed the 36 permission
work-count, 489 AppOps and 95 permission-state/consent contracts. That union is
not included in this change.

PR #73 replaces the FILE-address registry with a shared POSIX stream owner.
This optimization targets the current main architecture. If that PR lands
first, the cache must be integrated with its owner and retirement mechanism;
two competing stream-lifetime registries must not be retained mechanically.
