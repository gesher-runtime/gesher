# Gap ledger after session resume (2026-10-27)

This scheduling ledger preserves the aggregate owner goal: every still-open
roadmap, handoff, status and ADR contract, followed by verified main/release0.1.0.
It does not replace or narrow those sources. The latest session checkpoint
supersedes old handover tables; accepted evidence supersedes a historical
implementation absence only for its explicitly verified scope. No overall
completion percentage or deduplicated semantic-gap count is established.

## Counted baseline and acceptance states

At integration3fb8255d the unchanged native-api-coverage.json contains1157
historical missing-symbol rows;145 have subsequent scoped verification and
1012 lack it. These are export-inventory rows, not semantic gap counts.

| Recorded owner | Baseline rows | Rows without scoped verification |
| --- | ---: | ---: |
| native3 |724|698|
| media3 (media4 continuation) |313|170|
| net4 |84|82|
| surfaces2 |25|25|
| web3 |7|7|
| exec3 (exec4 continuation) |4|3|

The integration docs contain six unchecked roadmap rows and one unchecked
Windows4 physical-input row. Most remaining requirements are prose, so these
seven occurrences are not an aggregate-gap denominator. The six roadmap rows
are AOT3, Native3, Exec3 compatibility, aggregate video, DXVA and physical/
remaining media. Their linked briefs and ADRs remain normative.

Use pending, scoped-CI, full-verified, accepted/integrated as distinct states.
Full acceptance requires every job success/skipped, both Windows whole0,
actual new runtime output, installed package UI/input/uninstall and both
required e2e sets0unexpected. Document exact source/run and verify docs-only
head drift. A changed combined code tree needs full integration before another
code merge; identical verified non-doc trees need no redundant full.
Preserve every old inventory field/proof; never promote symbol presence to
semantic or hardware completion. Compilation/native/JVM/C# fixtures are CI-only.

## Current bounded leaves and dependencies

| Slice | Current evidence/state | Next acceptance gate |
| --- | --- | --- |
| Net4 legacy resolver |Accepted/integrated3fb8255d; full40519019567/f3631763; frozen50017009 docs-only;16 runtime contexts plus oracle/cleanup=18 top PASS each |Merged non-doc tree exactly equals verified source;82 inventory entries and other resolver/socket contracts remain |
| Libcore copy |Accepted full40550474909/944a0eef: both genuine15 child groups/final verdict/whole0, package23 and both ten-app/transport/association/shortcut e2e0; frozenba967dea docs-only |Merged non-doc tree equals944; no extra rule-g full. Induced short-write/EINTR/concurrent cancellation/other IO-native gaps remain; [exact report](tracks/libcore-io-final.md) |
| Exec ordered spawn |Scoped40549932840/00c7af77 green, absolute and relative Dir each4 required ABI action/no-console passes per host |Full acceptance with current integration and fixed Web3; no production Unicode-chdir change justified |
| Web3 teardown |Recurring original failures; observer40550620230/f4b5bdf5 pending with exact96b runtime producer identities |Failing PC/WCT/native stack, justified fix and full regression; observer success alone is no fix proof |
| AOT3 graph/Main loader |Component40524933356/1120 green21 scoped+15 graph each |Corrected full after shared Web3; resource/DLC/secondary/staticSDK remain |
| Locale/Wide/FPCR/FPSR union |Locale full405160 red solely Web3; scoped work passes |Coherent whole full; original frozen FPSR proof retained |
| ARM varargs API3 |Correction40528739976/928a09b7 scoped green; coherent40549806536/c7a43653 pending |Matching fresh host+DLL, jar/boot provenance, actual ART/APK/copiedJNI-V/FPCR/FPSR; only then hand exact prerequisite to Media |
| ARM env85 |Source54cccc22 prepared; no runtime acceptance |Peer/source review, coherent actual CI and later full |
| NDK muxer |Original full405277 red startup cwd defect; actual packet baseline405529 proves101 correct Opus payloads plus malformed empty EOS/5760 extra PLC frames. Corrected component40555035592/56899f17 green both8-context relative/absolute suites,18parts/12FD/10outputs/10decodes, CRC/granule/0-1-2 boundaries |New distinct payload-EOS source d55212cf includes acceptedb323; full40556596204/component40556590031 pending. API2 pair is matching bounded evidence only; exact verified Native coherent host+DLL must precede qualifying changed-union full and integration |
| Surface/AAudio/notice |Full405078 red; atomic logger compile40526285481 only |Native coherent prerequisite, actual80+86 component, retained legacy logger/pixel/phase gates and corrected Web3 in broad full |
| CI process helpers |Scoped40526183946 green four real processes each; no full |Legacy SurfaceRun/test76/other waits still open; current shared RunTimed bounded candidate must pass coherent full |

## Category ownership and closure plan

All links below retain their complete required-work/status sections and the
ADRs they cite. A row summarizes scheduling, not the source's entire scope.

| Category / canonical sources | Still-open families and dependencies | Next implementable slice / evidence |
| --- | --- | --- |
| [Services3](tracks/services3.md), services/device/lifecycle status; ADR0069 and0012/0019/0024-26/0038/0043/0046/0051 |Actual per-app UID/package/permission/installer authority; service/provider/task/storage/appops/widget/search; standby/alarms/time; GNSS/geofence/sensor/camera/geocoder; prerequisite for privileged locale/media/property boundaries |Unstarted. Establish real identity/permission authority with allowed/denied package/UID/device callers; then permission/export/provider tests. Resolve geocoder cancellation independently. Physical sensors/GNSS/camera interoperability still requires actual hardware evidence |
| [Binder2](tracks/binder2.md), binder/input status; ADR0068/0015/0027 |FD/FDA ownership, SG PTR/HIDL fixups, security context, freeze/priority, readiness/concurrent waiters, cross-process InputChannel/socket; shares POSIX/identity authority |Unstarted. Audit existing FD duplicate/close/error behavior with independent client/service lifetime controls, then real InputChannel/socket transport. Windows process/thread effects must be observable, not accepted commands |
| [Surfaces2](tracks/surfaces2.md), graphics status; ADR0063/0006/0030/0056 |Java EGL/GLES/GLSurfaceView; external-OES SurfaceTexture; transactions/layers; JPEG_R/mapped Bitmap; SkiaGL composition; native/hardware buffers/fences/choreographer; WebView functors/shader cache/GPU budget |Unstarted. Pinned Java EGL/GLES and small GLSurfaceView lifecycle/pixel fixture first, both modes/ABIs. WARP covers software contracts; real GPU/fence/hardware paths require real GPU evidence, retained explicitly |
| [Web4](tracks/web4.md), webview status; ADR0072/0041/0050/0058 |Child frames/origins/permissions, transferred MessagePorts, environment restart/reverse bypass, external cert keys/SafeBrowsing/errors, geometry/cutouts/zoom/capture/picker/advertised features |Unstarted; depends on real Web3 teardown fix. First child-frame listener/document-start identity+origin allow/deny with actual callbacks on both WebView2 versions; independent UIA, both modes and WebView app e2e |
| [Native3](tracks/native3.md), native-libs/posix/framework status; ADR0062/0011/0014/0017/0057 |698 unverified rows plus libc/libm/binary128/wide stdio/collation/zlib; LLP64; allocator/pthread clocks; record locks/inotify/shared mremap/permissions/epoll; JVMTI/namespaces/Vulkan |After coherent varargs/env, allocate bounded ABI/POSIX slices with actual two-ABI ART/ELF/APK and cross-process oracles. Upstream library suites/real agent execution remain explicit requirements |
| [ARM64-2](tracks/arm64-2.md), arm64 status; ADR0067/0047 |Guest JIT/self-modification, signals/ucontext return, LSE/order, HFA/mixed registers, full fenv/binary128, cache/stack bounds and benchmark |Current varargs/env work is bounded. Retain independent QEMU/native references and actual host/DLL identity; later full guest-code/concurrency/control tests, not flags-only claims |
| [Net4](tracks/net4.md), networking status; ADR0070/0009/0037 |82 inventory rows, further resolver/wire/per-network contracts, socket shared mode/ancillary/families/timeouts, IPv6-only DNS, Wi-Fi/VPN/captive/keepalive/QoS, crypto acceleration |Resolver leaf accepted only. Next bounded resolver or shared O_NONBLOCK alias/closure contract with real peers. Physical WLAN consent, IPv6 topology and captive transitions need their actual environment; crypto needs upstream tests/comparison |
| [Exec4](tracks/exec4.md), exec/posix status; ADR0074/0055/0060 |Broader shell and PID-preserving exec, console groups/signals, binary logd/events, TLS variants, fork-before-exec/static ELF, property authority/report/poke |Finish ordered spawn full after Web3; later one real behavior per slice. Shared property authority depends Services3/Binder2; real console and executable/TLS semantics need actual controls |
| [AOT3](tracks/aot3.md), compiled-code status; ADR0065/0031/0033/0036/0054 |After-resource ordering, DLC app/image, secondary dex/ownership/update, staticSDK/factory loaders, exact graphs and stale rejection |Corrected graph full first. Use matching rebuilt jars/images and oatdump/actual compiled execution; graph controls alone do not close resource/loader families |
| [Locale2](tracks/locale2.md), locale status; ADR0066/0044/0049/0053 |Verified persistence candidate still lacks coherent full; installer/privileged identity broadcasts, real display-language sign-in, custom regional formatting and native time-zone contracts |Full after Web3; coordinate Services3 authority. Controlled NLS test is distinct from actual display-language/sign-in event evidence |
| [Windows4](tracks/windows4.md), windows/input status; ADR0061/0039/0053 |Physical touch keyboard, arbitrary third-party candidate discovery and remaining rich TSF/input limits |Current bounded TSF/display leaf already accepted. Actual keyboard/candidate/manual desktop evidence remains required, including supported callback/geometry behavior |
| [Media4](tracks/media4.md), media/API inventories; ADR0064/0042/0052/0059 |170 unverified rows plus semantic NDK, SMTC/Session2/permission/volume, recorder/error/route/xrun/capture, containers/extractor, legacy camera/persistent surfaces/encoders/DXVA/DRM/effects/profiles/MediaStore |Muxer startup diagnosis first, then8x18parts/12FD/10decodes each host. Surface gets exact coherent Native prerequisite. Physical microphone/loopback/removal, real desktop SMTC, MF-positive encoding and GPU acceleration need positive actual evidence |
| [Release](tracks/release1.md), installer/e2e status |0.1.0 version/receipt/promotion/notice preparation only |Every gap closed; accepted coherent full, actual installed notice/version/UI/input/uninstall; main PR/merge under advance owner authorization; immutable assets/source/SHA/hash/provenance verified before publication |

## Audit expansion still required

Concrete unmet hardware/session inputs, owners, existing proof and observable
acceptance are retained in the [hardware evidence plan](hardware-evidence-plan-2026-10-27.md).
ADR0080/test87 is reserved to the Native POSIX/OFD record-lock slice by the
manager; no other worker may allocate it. Binder2 uses its existingADR0068
and reservedtest88. The next new allocation is ADR0081/test89, subject to
checking absence in current refs/files before reservation.

The prose contract list is not yet atomized/deduplicated. Before final
completion, reconcile every remaining/leftover/not-done/status statement in
all docs/ADRs against later exact evidence, including the framework native
registration/stand-in inventory and app/e2e status limits. Keep unresolved
statements pending and link each supersession to source/run; neither old
handover text nor this scheduling table silently closes an item.

## SG public transaction-prefix requirement: preserved open

Binder2 existing ADR0068/test88 still requires complete public transaction
publication/read_consumed and local/remote resource ownership evidence after
transport failure. Compile-only7e9549/40571824155 does not prove it; raw manual
handle and BR_NOOP-only prefix source controls do not substitute. Interrupted
WIPb52d0bda is preserved separately on wip/binder2-sg-relay-unverified after an
automatic content-filter interruption reported by the manager. This required
test remains unverified/open, not waived. Routine mode wiring proceeds from
known compiled source on wip/binder2-sg-mode-wiring; no aggregate Binder2 or
FD/permission/identity cleanup checkbox may be closed from that compilation.

## Incremental semantic reconciliation (2026-10-28)

Saved review checkpoints, reusing the existing prose index rather than
rescanning/counting it:

- C:/work/gesher-ci-review/semantic-gap-index-2026-10-28.json (navigation)
- C:/work/gesher-ci-review/semantic-gap-checkpoint-2026-10-28.json
- C:/work/gesher-ci-review/semantic-gap-exec-continuation-2026-10-28.json
- C:/work/gesher-ci-review/semantic-gap-services-frontier-2026-10-28.json
- C:/work/gesher-ci-review/semantic-gap-native-continuation-2026-10-28.json
- C:/work/gesher-ci-review/semantic-gap-net-continuation-2026-10-28.json
- C:/work/gesher-ci-review/semantic-gap-arm64-continuation-2026-10-28.json
- C:/work/gesher-ci-review/semantic-gap-process-ownership-2026-10-28.json

These cover selected AOT3/Exec4/Services3/Native3/Net4/ARM64 and process contracts,
with stable semantic IDs,
source/section/audit-row references, owner, state, dependency, exact scoped
evidence and concrete acceptance. Raw source paragraphs and inventory symbols
are not a remaining-gap denominator. No current checkbox or inventory proof
changes, aggregate count, all-gap completion or release claim follows.

AOT ordinary PCL/DLC shared-library graph proof450/full40557661255 is recorded
as a qualified older leaf awaiting current combined-source full. After-resource
ordering, secondary-dex ownership/update, static/SDK contexts and factory
loaders remain separate open contracts. Older blanket oatdump/cloud/startup
open wording is reconciled against their accepted bounded reports, without
reopening them or extending their proof to the remaining families.

Exec records distinguish PID-preserving exec, console interruption, binary
events/logd, broader shell/real umask, remaining executable TLS, general fork,
static executables, property authority/notifications and initial executable
UTF-8 environment behavior. Qualified ordered-spawn82/component40582080725
still needs actual canonical77/current-union full. Accepted registration and
shared-library live-environment slices stay historical scoped supersessions.
Shared identity/permission dependencies are cross-references, not duplicated
implementation gaps in each consumer. Historical intermittent-crash causality
and untracked adopted-exit limitations retain explicit unresolved classification;
neither invented causality nor fabricated successful status is permitted.

All other source families remain unreconciled. The primary compiler-provenance
correction/preflight and changed-source full take priority over further mapping.

The later Native continuation records the record-lock family as one semantic
contract with explicit nonblocking/blocking/close/ownership dimensions; current
source fixes and registry-seam controls do not qualify production fcntl.
Inotify cross-process events, shared file-backed remap, epoll/poll readiness,
actual JVMTI agents, namespace isolation and allocator accounting remain open.
Accepted width/current-CPU-clock history supersedes only its older scoped
pending/withdrawn wording. The remaining libc/libm/binary128/platform bridges,
clock/permission/signal/metadata/Vulkan families still require reconciliation.
No Native87 source joins the primary union or receives CI acceptance here.

## Release preparation feasibility, not readiness (2026-10-28)

C:/work/gesher-ci-review/release1-next-candidate-feasibility-2026-10-28.md
records a short saved-source review of release1fc3e0a8a. The current release
guard's required-job/step manifest predates canonical FPSR reference, both
query matrices and production seal/hook/query critical steps. A later separate
candidate must reconcile that graph, explicit0.1.0 PE/filename selection,
actual installed OpenSL source-hash equality and post-smoke authenticated
receipt. Preserve reviewed source-pin/reauthentication/attempt/asset guards,
real candidate dry-run and eventual both-runner released-byte installation/
e2e cleanup. The old release helper's bounded wait and reader/task/pipe/process
ownership also needs actual verification. No code/version/tag/main/asset or
publication action occurred. Existing package23 and offline promotion review
do not prove new notice/version/publication. All-gap/hardware gates remain.

## Process cleanup evidence boundary (2026-10-28)

Current frozen30db's process84/25 controls establish bounded wait/status/output
behavior and actual process SafeHandle disposal, not reader/task drain or
pipe-handle disposal. Windows' official.NET9 source review found that user-
accessed StandardOutput/Error readers remain caller-owned after Process.Dispose.
The separate20/21 component40597998338 at1a3b366f is green43controls/host
(18new plus identical25 retained), but its scope is still parser/bounds/
outputstatus/process SafeHandle. Both retained readersFalse cases prove
incomplete-output classification; they are not task/pipe-drain proof. Explicit
reader/task/pipe ownership follow-up remains source WIP and excluded from30db.
Even a green current full must not close that broader cleanup contract.
C:/work/gesher-ci-review/semantic-gap-process-ownership-2026-10-28.json records
the source/evidence/owner/dependency/acceptance boundary and release consumer.
