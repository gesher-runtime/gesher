# Systematic compatibility inventory, 2026-11-05

The measurements below describe the initial inventory snapshot. Subsequent
fixes and the final combined candidate are covered in the
[integrated stability QA report](stability-qa-2026-11-05.md), including updated
API member counts and explicit remaining limits.

The objective is to find missing Android contracts before a particular app
hits them. The audit therefore uses Android API references, the actual boot
class path, native library lookup, Binder/JNI implementation inventories and
the whole repository documentation set. App failures are additional evidence.

This is a central map of measured structural differences and known open
behavioral families. It is not a claim to have discovered every possible bug.
No sum of class, export, default-method and documentation counts measures
semantic compatibility: these sets overlap and include optional interfaces.

The inventory totals below retain their original hashed candidate snapshot.
Subsequent source fixes restore network/BLE value families and MediaStore
contracts; their isolated value tests pass 124 assertions. This does not update
those structural counts or establish the complete current app candidate's
compatibility. See the verified client-contract slice below.

## Reference and evidence rules

The managed reference is the installed SDK 36 `android.jar` (Android 16).
The native reference is official NDK r30 API 36 stub libraries, both x86_64
and arm64-v8a, obtained through the user's authorized remote-download route.
The complete upstream archive was checked against its published SHA-1;
individual extracted reference files have SHA-256 identities.

Android's [CDD](https://source.android.com/docs/compatibility/16/android-16-cdd)
distinguishes public API availability from behavioral compatibility.
[CTS](https://source.android.com/docs/compatibility/cts) includes API signature
and behavior tests; an API declaration alone cannot prove either behavior or
hardware interoperability. The [NDK library list](https://developer.android.com/ndk/guides/stable_apis)
provides a reference independent of the apps selected for smoke tests.
These are useful compatibility references for this Windows implementation,
not a claim of Android certification.

Every conclusion should retain one of these states:

| State | Meaning |
| --- | --- |
| Measured absence | The exact reference entry was not found in the exact inspected binary/class path. Alternate providers and optionality still need classification. |
| Source-confirmed difference | Current code explicitly returns a default, rejects an operation, or implements different semantics. A failure in a particular app is a separate question. |
| Documented open contract | Repository requirements retain the obligation; current complete behavior has not been established here. Later scoped implementations are preserved. |
| Historical observation | A previous runtime/log failed; the current source or binary can differ. |
| Verified slice | A named positive/negative test passed on identified bytes. Its scope is retained. |
| Unassessed | No trustworthy conclusion yet; this must not silently count as implemented. |

The installed runtime, current main working tree and isolated candidate are
different inputs. In particular, the main tree contains concurrent microG,
identity and lifecycle work. The reviewed native candidate is `564118b1`;
its new Windows fatal-context probes pass. Neither its source changes nor the
Java candidate have been installed into the user's shared running device.

## Reproducible inventories

| Inventory | What it examines | Result/artifact |
| --- | --- | --- |
| Java class availability | SDK 36 public class declarations versus actual DEX class definitions in all five selected boot JARs, then replacement candidate app JAR | 6,227 reference classes; 982 absent from the installed selected boot path, 977 after the candidate replacement. `out/compat-audit/java/java-class-availability.json` retains each class, provider taxonomy and input hashes. These are class-presence differences, not 977 defects or method/signature coverage. |
| Java member descriptors | All public/protected SDK field, constructor and method declarations, resolved through actual runtime superclass/interface definitions in boot order | 90,630 declarations checked. Installed: 353 descriptors absent on present classes; candidate: 343 (209 fields, 134 methods). Members on absent classes are separate: 8,693 installed / 8,574 candidate. Another 498 rows have possible modifier/access differences, not confirmed defects. `out/compat-audit/java-members/java-member-summary.json` and the per-declaration JSONL retain details, hashes and conservative resolution rules. |
| Native API availability | Every dynamic export in the pinned API 36 libraries, for both ABIs, looked up in the newly built runtime | 4,720 rows per ABI: 2,774 found and 1,946 absent in each. `out/compat-audit/native-api36/native-api36-presence.json` preserves each row, library-open status and hashes; `out/compat-audit/reference/` preserves the reference. This is symbol presence, not an invocation/ABI/namespace test. |
| Binder services/defaults | Current source Default/Stub subclasses and direct instances, compared with the supplied compiled AIDL interface input | Over 60 classes/instances; approximately 770 explicit methods and 3,000 inherited Default methods; approximately 45 central registration rows. Exact source-dependent counts and individual methods: `out/compat-audit/systematic-services-jni-2026-11-05.json`. Inherited comparison is by method name, not overload-level proof. |
| JNI registration | Pinned AOSP AndroidRuntime registration list versus local registrations, generated and attach fallbacks | 174 upstream rows, 124 symbol-name candidates and 716 fallback rows. Alternate registration names/order and later overrides prevent treating these as failure counts. Same service/JNI JSON. |
| Whole documentation set | Markdown subsystem documentation under `docs`, including all 75 ADRs, status and historical handoffs; generated audit descriptions are excluded | Snapshot: 241 documents, 402 sections containing gap/open/limit markers, zero read/cap errors. `out/compat-audit/doc-contract-index.json` hashes every input. These are navigation candidates, including superseded text, not 402 defects. |
| Existing runtime evidence | Bounded technical identifiers from all available top-level logs, excluding mirrored logcat | Initial scan: 40 logs, 135 identifiers. `out/compat-audit/compatibility.json`. Reports contain technical identifiers rather than raw account/input data. Current source status remains unassessed until checked. |

The deeper [service/JNI report](systematic-services-jni-2026-11-05.md) explains
the reviewed high-value families and pitfalls in the raw counters. The
[Java/native reference report](systematic-java-native-api-2026-11-05.md) records
the precise lookup/reference identities and reproduction commands. The
[audit guide](../compatibility-audit.md) documents tools and regeneration.

The candidate class-presence differences split into 285 connectivity-module,
230 other Android framework/module, 207 Health Connect, 154 AdServices,
56 Bluetooth, 33 NFC, 10 SDK sandbox and two annotation classes. The 1,422
reference java/javax/dalvik classes are all present on this boot path; their
member scan found no absent descriptors on those present core classes, while
behavior still needs separate verification. Examples absent from
that snapshot include AppSearch value/session types, BluetoothGattCallback,
MacAddress and SocketKeepalive. MacAddress has since been restored and tested
in the isolated source candidate. A value or callback type can be needed during
class construction even when the app later checks for absent hardware.

## Verified client-contract slice

Pinned Android 16 r4 implementations restore MacAddress, StaticIpConfiguration,
IpConfiguration, DHCP and L2CAP values, DNS-SD requests, BLE filters/settings/
advertisement builders and periodic reports. The local request implementation
now retains subscription filters, specifiers and transport parcel values,
uses wildcard-aware matching, and applies VPN transport session redactions.
[Source and test notes](../../app/java/android/net/VALUE-SOURCES.md) and
[BLE source/test notes](../../app/java/android/bluetooth/README.md) preserve exact
upstream URLs, source hashes and deliberate adaptations. Bluetooth still has
no adapter; the tests do not advertise a hardware capability.

MediaStore now includes its pinned metadata/query/intent constants, Audio
collection contracts, URI helpers, legacy search keys, and actual resolver
query/bitmap/thumbnail/playlist helpers. The
[MediaStore contract report](mediastore-contracts-2026-11-05.md) bounds the
restored API surface and backend obligations. It supplies no MediaProvider,
cloud media, scan index or permission-prompt activity.

Four separate-process tests passed together with isolated ART data and no
Binder connection: NetworkValueContractTest (30 assertions),
BluetoothValueContractTest (31), MediaStoreContractTest (25) and
NetworkRequestValueContractTest (38). They cover real parsers, validation,
positive/negative matching, complete parcel consumption, redaction, URI
construction and resolver forwarding. Exact runtime/candidate/input/log
hashes are retained in `out/stability/java-values-candidate/qa-receipt.json`.
The tests use a private DEX ahead of the prior app candidate on the boot path;
they do not substitute for a final whole-app build or end-to-end hardware/
provider testing. The installed shared app and its processes were unchanged.

The largest native lookup differences are libc (437), libicu (306), Vulkan
(251), libmediandk (197), libandroid (156), libbinder_ndk (150) and libm (133)
per ABI. Some complete sonames cannot be opened, including camera2ndk,
binder_ndk, ICU, Vulkan, neuralnetworks, amidi, sync and OpenMAX AL. Conversely,
all checked EGL/GLES2/GLES3 names resolve. None of those positive lookups proves
that its parameters, callbacks, rendered pixels or device backend work.

## Prioritized behavior map

Priority reflects likely breadth and severity: P1 can cause startup failure,
indefinite waits or loss of core operation; P2 affects feature families;
P3 needs a specific device, topology or advanced use. It is an engineering
ranking, not a measured crash probability. A family can contain implemented
and unimplemented methods; the evidence column bounds the assertion.

| ID / priority | Family and concrete remaining question | Evidence/state and verification needed |
| --- | --- | --- |
| S01 / P1 | Default service returns and asynchronous completion | Source-confirmed: inherited Binder Default methods can return null/0/false or never call a supplied callback. Probe constructor invariants, success/error completion deadlines and observer removal. [StandIn](../../app/java/com/gesher/server/StandIn.java), [service audit](systematic-services-jni-2026-11-05.md). |
| S02 / P1 | Service construction and manager dependencies | Source/installed skew is real: Phone Doctor reached a null DeviceIdleManager. Current deviceidle registration is still a Default controller. Instantiate all exposed managers and validate required dependency objects, then each policy contract. [Services](../../app/java/com/gesher/server/Services.java). |
| S03 / P1 | Activity, task, service and provider lifecycle | Important paths exist; remaining callback, death/reconnect, launch-mode, exported-component and process-lifetime contracts need method-level tests. Current independent lifecycle work must be integrated and tested rather than counted as complete from source. [lifecycle](../lifecycle.md), [Services3](../handoff/tracks/services3.md). |
| S04 / P1 | Settings and permission activities | Only selected actions have desktop fallbacks. Battery exemption, app details, all-files access and generic Settings/capture intents can remain unresolved without a matching APK. Inventory intent outcomes and return a correct failure or implement an actual desktop policy. [service audit](systematic-services-jni-2026-11-05.md). |
| S05 / P1 | Document picker and URI lifetime | A Windows picker exists, but file:// results do not establish DocumentsProvider IDs, persisted tree grants, MIME/multiple-selection contracts or cross-process URI ownership. Test read/write/reopen/revoke across process/server restarts. [service audit](systematic-services-jni-2026-11-05.md). |
| S06 / P1 | Identity, permissions, installer and package authority | Concurrent source work adds authority checks; no blanket old 'all permissions granted' conclusion is made. Test permitted and denied callers, shared UID/package mapping, install source and provider/URI grants on a coherent exact build. [Services3](../handoff/tracks/services3.md), [requirement matrix](../handoff/requirement-matrix-2026-10-29.md). |
| S07 / P2 | Software services independent of hardware | DevicePolicy, Shortcut, UsageStats and DropBox have empty Default subclasses in the source snapshot; SettingsProvider persistence/generations and AppOps history/observers remain limited. App widgets are advertised, while the service explicitly hosts no widgets and returns empty collections. Inventory useful desktop behavior and capability answers. [service audit](systematic-services-jni-2026-11-05.md), [feature declarations](../../app/java/com/gesher/server/PackageManagerService.java), [widget service](../../app/java/com/gesher/server/DeviceServices.java). |
| N01 / P1 | Actual JNI method bindings | Successful android_runtime loading can skip registration when its class is absent. LocalSocketImpl, MemoryFile/MemoryIntArray and Scrypt are named candidates; verify exact class+signature binding and behavior rather than matching only registration function names. [JNI audit](systematic-services-jni-2026-11-05.md), [framework natives](../framework-natives.md). |
| N02 / P1 | Public NDK library/export and ABI coverage | Refresh all API 36 sonames/exports, then test each absent or stubbed family. Correct symbol lookup must be followed by layout, error, callback and ownership checks on both ABIs. [Native3](../handoff/tracks/native3.md), [native libraries](../native-libs.md). |
| N03 / P1 | epoll semantics | Source-confirmed: EPOLLET is treated as level triggering; nested epolls are rejected; a registration is forgotten on closing its registered fd rather than the last open-file alias. Test aliases, nesting, polling epoll itself and concurrent waiters. [epoll source](../../compat/posix/src/epoll.cc). The basic Telegram-style event loop passing does not close these. |
| N04 / P1 | Socket ancillary transport and readiness | Current sendmsg ignores control data and recvmsg returns no ancillary data. Check SCM_RIGHTS, packet-info and urgent readiness against required contracts. Shared nonblocking descriptions and MSG_TRUNC now have code: old blanket 'missing' wording is superseded for their source presence, not full semantics. [socket source](../../compat/posix/src/socket.cc), [fcntl source](../../compat/posix/src/system.cc). |
| N05 / P1 | Blocking file record locks | Current fcntl routes nonblocking POSIX/OFD lock operations; F_SETLKW/F_OFD_SETLKW are not in that production dispatch. Blocking/wakeup/signal/close ownership remains distinct from working nonblocking tests. [system source](../../compat/posix/src/system.cc), [requirement matrix](../handoff/requirement-matrix-2026-10-29.md). |
| N06 / P2 | Shared file remapping | SharedAlias now supports a bounded existing-view alias path; ordinary file-view resizing still explicitly fails. Do not repeat the obsolete claim that every file-backed mremap is absent. Test growth/shrink/move, sharing and cleanup. [mman source](../../compat/posix/src/mman.cc). |
| N07 / P2 | Filesystem notifications, proc and permissions | Known inotify cross-process/open/access/close/moved-directory limits; proc metadata and NTFS permission semantics differ. Verify each against current code and real second processes. [POSIX gaps](../posix.md#known-gaps). |
| N08 / P2 | Native ABI, libc/libm and allocator | Keep accepted Unicode width, clock, FPCR/FPSR and varargs proofs. Remaining binary128 math, stdio/locale layouts, allocator accounting and upstream library behavior require their own tests; exported names alone are insufficient. [Native3](../handoff/tracks/native3.md), [foundation](../handoff/tracks/native3-foundation.md). |
| N09 / P2 | ELF namespaces and JVMTI | System V agent tables/callbacks and real isolated namespace lookup remain documented obligations. Test an actual agent and conflicting dependency namespaces. [Native3](../handoff/tracks/native3.md). |
| A01 / P1 | ARM64 guest-generated code | Guest JIT/self-modifying code, executable-page transitions and translated-block invalidation remain an explicit open family. [ARM64](../arm64.md#not-yet-done), [ARM64-2](../handoff/tracks/arm64-2.md). |
| A02 / P1 | ARM64 signals and callbacks | Deliver true guest siginfo/ucontext and honor register edits on return; host-context delivery is not equivalent. Also test HFA/mixed-register exhaustion and nested callback marshalling. [ARM64-2](../handoff/tracks/arm64-2.md). |
| A03 / P2 | ARM64 atomics, floating point and resource limits | LSE is hidden by capability reporting in documented source; verify supported instruction/capability consistency and contention. Accepted FPCR/FPSR slices do not close host CRT/libm/traps, binary128 or cache/stack limits. [ARM64-2](../handoff/tracks/arm64-2.md). |
| G01 / P1 | GL lifecycle and SurfaceTexture external-OES | Java EGL/GLES natives already exist. Remaining renderer-thread/context-loss/pause/resize and attach/detach/updateTexImage/matrix/timestamp behavior needs pixel+callback fixtures. [Surfaces2](../handoff/tracks/surfaces2.md), [ANGLE](../angle.md). |
| G02 / P2 | Composition, child surfaces and storage | SurfaceControl alpha/transforms/layers/clipping/ownership, SkiaGL popup/readback, JPEG_R and mapped Bitmap storage remain explicit separate contracts. [Surfaces2](../handoff/tracks/surfaces2.md). |
| G03 / P2 | Hardware buffers, fences and rendering backends | Validate storage/usage/format/stride/ownership/fences, HardwareBufferRenderer, WebView functors, shader persistence and GPU budgets. Vulkan is a separate backend contract; a missing-device result does not establish an implemented loader. [Surfaces2](../handoff/tracks/surfaces2.md), [hardware plan](../handoff/hardware-evidence-plan-2026-10-27.md). |
| M01 / P1 | Native codec/extractor/muxer/image/protected media | Format and several muxer/OpenSL slices have accepted evidence; remaining NDK exports and actual state/data/error/ownership behavior are separate. Use fresh API export rows rather than the historical 313/136 counters as a current total. [Media4](../handoff/tracks/media4.md), [media API inventory](../media-api-inventory.md). |
| M02 / P1 | Audio route, disconnect and error completion | Documented AAudio callback/policy/route gaps and AudioRecord input-device behavior can silently accept unsupported operations. Test close/unplug/route transitions, callback cancellation and accurate errors. [Media4](../handoff/tracks/media4.md). |
| M03 / P2 | Session/media keys and desktop integration | Legacy session IPC is implemented; SMTC, receiver fallback, Session2, user/listener boundaries and full OpenSL effects/configuration are separate retained families. [Media4](../handoff/tracks/media4.md). |
| M04 / P2 | Containers, hardware codec and DRM | Retain later WebM interleaving evidence rather than declaring it wholly absent. Remaining formats, protected media, positive hardware decode/encode and driver resource lifetime require separate tests. [media status](../media.md), [interleaving proof](../handoff/tracks/media4-webm-interleaving-409530.md). |
| W01 / P1 | WebView frame/origin and port lifecycle | Child-frame listener/script/geolocation identity and permission, transferred MessagePorts and owner teardown remain contracts beyond main-frame/string tests. Later frame work has bounded/failed evidence, not blanket completion. [Web4](../handoff/tracks/web4.md), [requirement matrix](../handoff/requirement-matrix-2026-10-29.md). |
| W02 / P2 | WebView environment, certificates and UI | Late proxy/environment restart, reverse bypass, external certificate keys, SafeBrowsing callbacks and remaining geometry/zoom/capture/document-picker behavior require actual effect tests. [Web4](../handoff/tracks/web4.md). |
| R01 / P2 | Advanced resolver and network policy | Preserve accepted legacy resolver and network-binding slices. Raw DNS beyond A/AAAA, IPv6-only/per-network behavior, Wi-Fi/VPN/captive transitions, QoS/keepalive and crypto acceleration remain bounded separate requirements. [network](../network.md), [Net4](../handoff/tracks/net4.md). |
| E01 / P2 | Process/exec/shell and property semantics | PID-preserving exec, real fork-before-exec, static ELF, console process groups/signals, remaining TLS, binary logd and shell conformance remain documented contracts. Property notifications have later work; caller policy still requires authority tests. [Exec4](../handoff/tracks/exec4.md), [continuation](../handoff/session-2026-10-29-continuation.md). |
| B01 / P1 | Binder transport ownership and authority | Retain accepted ordinary SG/read-only copyout component evidence. Failure publication, concurrent protection changes, FD ownership, HIDL/security contexts, freeze/priority and private bootstrap authority remain separately bounded. [Binder2 proof](../handoff/tracks/binder2-sg-ordinary-component-proof.md), [read-only proof](../handoff/tracks/binder2-stable-readonly-component-proof.md), [matrix](../handoff/requirement-matrix-2026-10-29.md). |
| C01 / P2 | AOT/class-loader cache invalidation | Ordinary PCL/DLC graphs, cloud profiles and oatdump have scoped proofs. Secondary-dex ownership/update, filesystem races, resource ordering, factory/static SDK loader contexts and stale-artifact rejection need exact-image positive/negative tests. [AOT3](../handoff/tracks/aot3.md), [matrix](../handoff/requirement-matrix-2026-10-29.md). |
| L01 / P2 | Locale, time zone and configuration | Persistence has scoped evidence. Privileged caller/broadcast policy, actual Windows display-language sign-in, native time-zone changes and custom regional formats remain distinct from controlled NLS tests. [Locale2](../handoff/tracks/locale2.md), [locale](../locale.md). |
| I01 / P2 | Input, accessibility and geometry | Existing TSF/IME/accessibility bridges are real. Rich formatting/object insertion, scale/rotation geometry, arbitrary third-party candidates and physical touch-keyboard behavior require further tests. [input](../input.md), [services](../services.md#accessibility-web3-track-adr-0058), [hardware plan](../handoff/hardware-evidence-plan-2026-10-27.md). |
| H01 / P3 | Hardware discovery and capabilities | Camera reprocessing/offline/extension/torch, GNSS/geofences/sensor direct channels, actual audio/GPU/WLAN and positive geocoder behavior need both software contracts and real environment evidence. No physical radio/vibrator is not by itself a defect; advertised features and error callbacks must agree. [devices](../devices.md), [hardware plan](../handoff/hardware-evidence-plan-2026-10-27.md). |
| D01 / P1 | Actionable crash diagnostics | The old Windows ART report omitted the fatal PC. Candidate patch 0771 now passes isolated ELF+host fault probes with nonzero RIP/RSP and host module offset. Native unwinding and the later Telegram SIGSEGV cause remain open. [Telegram evidence](telegram-native-network-2026-11-05.md). |

## How this becomes preventive testing

1. Pin exact source, native executable, boot JARs, API level and ABI. Keep an
   installed-build result separate from a candidate result.
2. Regenerate the class, NDK, JNI, service and document inventories. Classify
   optional modules, annotations, weak imports and explicit hardware absence
   before assigning work. Retain the separate Java member/descriptor comparison:
   a class-only scan cannot detect a missing method in a present class.
3. For each exposed manager/service, test construction, required object shapes,
   callback success/error within a deadline, cancellation, binder death and
   restart. Use strict-services in selected fixtures to expose an unexpected
   inherited fallback; do not turn it on indiscriminately for the user's apps.
4. Run reusable native/ABI and lifecycle fixtures, including two-process error
   and ownership cases. Add focused Android CTS tests where the contract can
   run on this architecture; running an entire device-oriented CTS suite is
   not a one-command substitute for Windows environment integration.
5. Retain small representative app smoke tests as a final layer. Real hardware
   tests remain distinct from synthetic providers or WARP.

The first work queue should address S01/S02/N01 (startup/null/callback
failures), N03/N04/N05 and B01 (wait/descriptor/ownership contracts), then the
measured API class/export families with the widest affected scope. Existing
working slices remain regressions. This sequence is intended to prevent
families of app failures without requiring the user to discover each one.

## Current verification and limits

The native runtime and supporting targets build locally with pinned LLVM/AOSP
inputs and an explicit local provenance mode, preserving the standalone font
checks. Initial actual socket, epoll, pthread-exit, libc and Binder-counter
regressions pass; libc and native2 regressions also pass through the matching
new ARM64 translator. The new fatal-register probes pass. These checks do not
constitute a full newly packaged release or close every family above.

VLC's search/pthread-cleanup fixes, OsmAnd BLE types, notification-settings
fallback and the Telegram-like event-loop regression have specific evidence in
[the app stability report](app-stability-2026-11-05.md). Telegram reached the
server/login-email stage, but its subsequent native crash is unresolved.
The user's running shared device was not closed/restarted, and no account
credentials were entered. No GitHub publication was needed for local building.

All 402 documentation marker sections remain available for later adjudication;
this report has reviewed the listed cross-cutting families, not independently
proved or closed every statement in every historical handoff. Generic/source
access rules beyond the member resolver, constant values, reflection/dynamic
code, silent wrong results and hardware behavior remain important explicit
limits of static presence inventories.
