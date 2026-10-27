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
| Libcore copy |Full40522376074/2913d484 green; actual15 child groups suppressed by successful CheckTimed |Logging-only4671d3dd retains all assertions and prints actual output; accepted integration merged; coherent full40550474909/944a0eef pending, require all15 groups each host |
| Exec ordered spawn |Scoped40549932840/00c7af77 green, absolute and relative Dir each4 required ABI action/no-console passes per host |Full acceptance with current integration and fixed Web3; no production Unicode-chdir change justified |
| Web3 teardown |Recurring original failures; observer40550620230/f4b5bdf5 pending with exact96b runtime producer identities |Failing PC/WCT/native stack, justified fix and full regression; observer success alone is no fix proof |
| AOT3 graph/Main loader |Component40524933356/1120 green21 scoped+15 graph each |Corrected full after shared Web3; resource/DLC/secondary/staticSDK remain |
| Locale/Wide/FPCR/FPSR union |Locale full405160 red solely Web3; scoped work passes |Coherent whole full; original frozen FPSR proof retained |
| ARM varargs API3 |Correction40528739976/928a09b7 scoped green; coherent40549806536/c7a43653 pending |Matching fresh host+DLL, jar/boot provenance, actual ART/APK/copiedJNI-V/FPCR/FPSR; only then hand exact prerequisite to Media |
| ARM env85 |Source54cccc22 prepared; no runtime acceptance |Peer/source review, coherent actual CI and later full |
| NDK muxer |Full40527788805/5d9e24d3 red, both Windows18fail; all8 contexts before markers,0/18parts,0/12FD,0/10outputs/decodes |Diagnose common startup fault (JNI0xC0000409, standalone1), then every context/gate; ARM-only correction is not established common cause |
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
manager; no other worker may allocate it. Next candidate is ADR0081/test88,
subject to checking absence in current refs/files before reservation.

The prose contract list is not yet atomized/deduplicated. Before final
completion, reconcile every remaining/leftover/not-done/status statement in
all docs/ADRs against later exact evidence, including the framework native
registration/stand-in inventory and app/e2e status limits. Keep unresolved
statements pending and link each supersession to source/run; neither old
handover text nor this scheduling table silently closes an item.
