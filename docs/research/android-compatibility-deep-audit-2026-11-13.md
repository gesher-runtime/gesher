# Android compatibility deep audit — 2026-11-13

This pass repairs independently reproduced network-value, Wi-Fi data, event-loop,
audio-state and native-window contracts. It also records remaining SDK and
service/JNI review queues. Work and publication identity: **palmoni5**.

Three investigation agents mapped Java/services, POSIX/bionic/ELF and
media/graphics/device boundaries. Separate implementers made the selected
repairs, followed by independent Java, native, media and integration reviewers.
QA found two additional regressions in the proposed changes; both were repaired
before publication. This is a repository-wide map and focused behavioral audit,
not a line-by-line proof or a claim of arbitrary-APK parity.

The [machine-readable inventory](android-compatibility-deep-audit-2026-11-13.json)
contains every absent SDK class, every absent declaration on an available class,
all modifier-review rows, all improved declarations, artifact hashes and the
service/JNI input hashes. These are structural counts, not application crashes.

## Baseline and avoidance of duplicate work

Baseline: PR [#12](https://github.com/gesher-runtime/gesher/pull/12) head
`45a278b4c506978f2a39c4070df00a19e303c908`, which already includes PR
[#10](https://github.com/gesher-runtime/gesher/pull/10) at
`165bcbcd7489a247d1f86a9e941121c17b8a9a1c` and PR
[#11](https://github.com/gesher-runtime/gesher/pull/11) at
`8d52fdca8241447ac4d0a83795a450aa92c45391`. All three were open at the start;
#10 and #11 merged during this audit.

The new work builds on those commits. Account/GMS/NFC/domain verification,
PendingIntent/observer/scheduler lifecycle, guest waits/TLS, filesystem
metadata/rename, HTTP/FD media sources and window retention remain owned by
their earlier changes. The new PR targets `main`, as required by the unchanged
coherent-TCP source policy. Until #12 merges, its inherited commits are an
explicit dependency, not a second implementation.

Both baseline and candidate `gesher-app.jar` were built afresh. Common core and
framework inputs were reused from verified run
[43302471272](https://github.com/gesher-runtime/gesher/actions/runs/43302471272).
The JSON records their actual hashes, the candidate Java source-tree identity
and separate baseline/candidate JAR hashes. Local inventory inputs are not
represented as a fresh full-platform build receipt.

## Repository map

The baseline contains **2,847 tracked files**: `tests` 1,104, `compat` 507,
`docs` 321, `tools` 317, `app` 313, `.github` 83, `probes` 83, `patches` 73,
`cmake` 33, root-level files 7, `config` 4, `assets` 1 and `java` 1.

| Layer | Main paths | Boundary followed |
| --- | --- | --- |
| Pinned platform/build | `aosp.lock`, `patches`, `cmake/aosp`, `tools/framework` | Selected AOSP sources, generated Binder defaults, registration and artifact identity. |
| App entry/framework values | `app/java/com/gesher/app`, `app/java/android` | Bootstrap, SDK descriptors, network snapshots and data contracts independent of hardware. |
| In-process services | `app/java/com/gesher/server` | Generated interfaces, callbacks/ownership, explicit and inherited defaults, Windows-backed services. |
| Host POSIX | `compat/posix/src` | CRT/Win32 descriptors, virtual objects, poll/epoll/select, vector dispatch and file identity. |
| Guest libc/ABI | `compat/bionic` | Argument/result widths, libc routing, timers, TLS, callbacks and unsupported process semantics. |
| ELF/translated ARM64 | `compat/elf`, `compat/arm64` | Relocation, symbol dispatch, guest TLS and x64-hosted translation. |
| Framework JNI | `compat/framework`, JNI registration tables | Native registration and Java/native ownership; a table entry alone is not behavior proof. |
| Audio/media | `compat/media/{audio,jni,player,video,encode,mux,ndk}` | Shared engine state, AudioTrack calls, SoundPool ownership and codec/source lifetime. |
| Native windows/graphics | `compat/media/surface`, `compat/hwui/window` | Producer/user/consumer sizes, allocation, damage, publication and real Win32 windows. |
| Devices/input/desktop | Device, camera, input and WebView adapters | Physical capabilities, timing, text composition, callback and browser integration limits. |
| Verification | `tests`, `probes`, `.github/workflows`, `tools/ci` | SDK callers, original-source controls, Linux references, declared adapters and Windows gates. |

## Fresh SDK and source inventories

SDK36 `android.jar` SHA-256:
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
The inventory examines the ordered five-JAR boot path and resolves inherited
public/protected declarations using first-provider-wins ordering.

| Measurement | Combined PR #12 baseline | Candidate |
| --- | ---: | ---: |
| SDK class definitions | 6,227 | 6,227 |
| SDK classes present on selected boot path | 5,315 | 5,317 |
| SDK classes absent from selected boot path | 912 | 910 |
| Public/protected declarations compared | 90,630 | 90,630 |
| Exact descriptor without reported modifier mismatch | 82,193 | 82,255 |
| Absent declaration on a present class | 584 | 548 |
| Declaration whose class is absent | 7,351 | 7,325 |
| Potential modifier mismatch rows | 502 | 502 |

The **62 improved declarations** comprise 36 members on existing classes and
26 declarations supplied by the two new SDK types, WifiSsid and SupplicantState.
The Wi-Fi query/constant additions contribute 33 of the former rows, and the
three WifiConfiguration IP/proxy methods contribute three. Other repairs change
behavior rather than descriptor availability.

The 910 absent classes comprise connectivity modules 264, other Android
framework families 212, Health Connect 207, AdServices 154, Bluetooth 41, NFC 21,
SDK sandbox 9 and AndroidX annotations 2. Modules or shared libraries outside
this boot path may supply some classes. An absent annotation is not automatically
a runtime linkage failure. The 7,325 declarations on absent classes are covered
by the complete class list, without duplicating every declaration.

Nine present descriptors still reference three absent types: GenericDocument,
Session2Token and IkeTunnelConnectionParams. The 502 modifier rows are 498
member-final and four class-final restrictions, requiring contextual review.
There are 35 duplicate class definitions resolved by boot order.

Do not compare the earlier follow-up's 536-member count directly with this
baseline's 584. The earlier report measured its named isolated candidate.
#10's partial NFC classes move some declarations from the absent-class bucket
to the missing-member-on-present-class bucket. Both sides above use the same
combined baseline.

The service/JNI source inventory found 70 service classes, 67 resolved
interfaces, 1,058 explicit methods and 2,767 **name-based** inherited-default
candidates. Explicit methods include 874 implemented-but-unverified paths,
12 empty collections, 80 default answers, 78 no-ops and 14 unsupported paths.
There are 51 direct registrations and 197 Context-key candidates. Of 174
upstream JNI registration rows, 50 have direct references, four alternate
references and 120 no direct reference; 523 stand-in methods are recorded.
All 81 inventoried source input hashes still match the candidate. Generated,
alternate or runtime registration can invalidate simple missing-reference
inferences, so these are review queues.

## Implemented repairs

| Area | Previous problem | Resulting contract |
| --- | --- | --- |
| Wi-Fi data | SDK callers lacked two data types and ordinary disconnected-state methods. | Pinned SSID/encoding and supplicant implementations; constructor-accurate WifiInfo queries/constants, without claiming a radio backend. |
| Routes | Inconsistent interfaces, accumulated same-key routes and lost MTU. | Adopt/rebind interfaces, reject mismatches before mutation, replace by destination/gateway/interface key, retain type/MTU through copies and local Parcel. |
| Address preference | Flags/scopes, private IPv4, IPv6 ULA and tentative/deprecated behavior differed. | Pinned constructor, scope and preference predicates, including multicast rejection. |
| Network snapshots | Equality omitted DHCP/WoL/private DNS and depended on order; arbitrary NAT64 lengths accepted. | Supported Android comparisons/hashes, DHCP local Parcel preservation and null-or-/96 validation without mutation on rejection. |
| Wi-Fi IP/proxy values | Public SDK calls could throw NoSuchMethodError. | Three public methods using existing IpConfiguration, retaining direct/PAC/null/copy and warning-and-retain invalid-proxy behavior. |
| epoll | Small result buffers indefinitely favored low-numbered ready descriptors. | Shared delivery cursor across aliases/waiters, preserving generation and ONESHOT checks. |
| writev | CRT writes bypassed eventfd counters, notifications and pipe wakeups. | Normal scalar route for non-sockets, existing socket gather, partial progress/device errors and empty-write access validation. |
| Host select/pselect | Duration overflow and lost residual budgets or premature slice completion. | Wide deadlines, repeated bounded poll slices, remaining select time/errno and unchanged const pselect timespec. |
| AudioTrack writes | Blocking priming/paused writes could hang; rapid restart hid interruption. | Inactive short progress and generation-based pause/stop/release interruption, with bounded real-thread checks. |
| Audio reset/loops | Stale flush notifications, static-stop loops and missed partial-loop endpoint. | Reset the source-defined state and wrap partial loops before the suffix. |
| SoundPool | Stale auto-pause ownership revived a later manually paused stream. | Consume ownership on explicit resume while preserving Android's distinct redundant-pause behavior. |
| Native windows | Producer size overwrote user defaults; reallocation/clipping missed invalidated pixels. | Separate dimension authorities, correct dequeue/query precedence, mixed-zero rejection and full/clipped damage based on buffer validity. |

Pinned Android compares LinkProperties addresses by IP identity, excluding
prefix/flags/scope. Its helper is asymmetric for duplicate IPs with different
prefixes. Independent QA reproduced the exact argument direction against the
unmodified AOSP helper and added both directional cases before approving the
repair. Invalid WifiConfiguration proxies are warned about and retained in
this Android release. Imported WifiSsid clones getter results but retains
upstream input-array aliasing; it is not an immutable defensive-input type.
See the detailed [Wi-Fi provenance report](wifi-data-contracts-2026-11-13.md).

Independent native QA found that the initial empty-write fast path wrongly
accepted read-only files and pipe read ends. The final implementation checks
the existing held handle's granted write access, guarded against descriptor
replacement, without performing artificial I/O. Shared fixtures cover empty
vectors and the absence of a phantom zero-length UDP datagram.

## Validation and execution boundaries

| Evidence | Result and meaning |
| --- | --- |
| SDK Wi-Fi caller | 133 assertions; old WifiInfo fails at getSupplicantState with NoSuchMethodError. |
| SDK network caller | 171 assertions; five original-source groups fail at intended old contracts; the pre-QA candidate fails the duplicate-address case. |
| Independent Java consumers | 31 extra assertions through actual WifiManager, StaticIpConfiguration and constructed-record ConnectivityManager conversion. |
| Native host models | 90 sanitizer checks: epoll 15, vector 47, select 28. Original sources fail 9/18/10; the intermediate vector candidate fails the four new access cases. |
| Real Linux shared fixture | 61 checks: alias fairness, vector/device errors, notifications, select, empty-write access and no phantom datagram. |
| Audio/SoundPool | 65 + 10 sanitizer assertions; original sources fail 21 + 7. Independent QA adds 15 audio edge cases. |
| Surface geometry/damage | 158 sanitizer assertions; old sources fail 34. Failed-query short circuits explain different old totals. Independent QA adds 22 edge cases. |
| Existing media suites | Timestamp 20, codec-window ownership 31 and media API 266 assertions remain green. |
| Build/provenance | Fresh app javac/D8 and service fixtures pass; public AudioStateContractTest compiles against SDK36 alone; upstream hashes independently verified. |
| Policy/integration | Four writev source-policy negatives and 150 coherent-context negatives pass. Workflow YAML, embedded Bash and actual PowerShell syntax checks pass. |

Host tests retain production code but replace declared Win32/JNI/audio-device
boundaries. Only LeakSanitizer is disabled locally because this executor cannot
perform its process inspection; the override is not embedded in CI. No host
test supplies a fake Parcel. Network/IP local Parcelable round trips do not
establish Android wire-layout or persisted cross-version interoperability.

Required actual executions on the published head:

- Existing Windows POSIX epoll and native ELF/ARM64 gates include the same
  shared event-loop fixture through actual host/guest routes.
- Track `96-deep-compatibility.ps1` runs Wi-Fi/network callers in ART/JNI,
  requiring successful process exits and `parcel=true`.
- The new surface executable uses actual QueueSurface and Win32Surface,
  a real hidden window and AOSP native-window dispatch.
- AudioStateContractTest runs actual AudioTrack JNI with a clocked null sink
  and isolated ANDROID_DATA. Real mixer progress precedes interruption;
  thread/process waits are bounded and resources are released.
- Track 96 participates in full runtime and FrameworkContractsOnly on Windows
  2022 and 2025. Early Linux gates retain sanitizers, the native reference and
  exact-source negative policy controls.

The exact published commit's workflow results are authoritative for CI status.
Source-policy and artifact-identity gates remain enabled. The allowed writev
region retains a reviewed full-file hash and negative controls rejecting
unrelated edits. No native Windows ARM64, physical WASAPI/camera/sensor or
complete Android API qualification is asserted.

## Remaining material work

| Priority / area | Concrete issue | Next evidence or design |
| --- | --- | --- |
| High: timerfd | Numeric-FD tracking in `compat/bionic/time_bionic.cc` does not supply coherent close/dup/reuse/rearm ownership. | Shared identity, alias-aware close, callback quiescence and deterministic reuse/rearm tests. |
| High: media release | Raw Codec/Recorder/SoundPool lookup versus release remains cross-cutting. Prior window retention covers one owned object. | Retain ownership across entrypoints and deterministic concurrent release/callback tests. |
| High: connectivity callback | A timeout checks a captured record, unlocks, then unregisters by callback key; reuse may retire a newer record. | Claim the exact record atomically and coordinate remote removal with deterministic scheduler tests. |
| High: providers/sync | Provider reference counts, Settings notifications, sync parallelism and retry decisions remain incomplete. | Real ContentResolver/Binder lifecycle, persistence, failure and recreation tests. |
| Medium: events/vectors | EPOLLET/EPOLLEXCLUSIVE, last-alias retention, aggregate widths/atomicity and read_iter semantics remain partial. | Shared open-description model and Linux differential fixtures beyond symbol presence. |
| Medium: waits/process/files | Atomic signal masks, futex modes, fork/mount/namespaces, nftw flags/cycles and path-based directory identity lack complete translations. | Explicit capability/error contracts and targeted Windows/Linux behavior tests. |
| Medium: module APIs | 910 absent classes, 548 absent declarations on present classes and three absent descriptor types remain. | Prioritize actual linkage and complete coherent data/service families; importing names alone is insufficient. |
| Medium: network builders | Capability/transport bounds and legacy specifier conversion differ from Android. | Validate public builders and specifier dependencies, ownership and callbacks together. |
| Medium: buffers/devices | External SurfaceTexture sampling, transforms/dataspaces/fences, multi-buffer queues, orientation and sensor batch/flush behavior remain partial. | GL/device oracles, actual timing, flush-marker and lifetime tests. |
| Platform qualification | x64-hosted ARM translation is not native Windows ARM64; input/browser/device semantics are broad boundaries. | Separate native-host gates and hardware/application matrices. |

These are actionable source findings and review targets with differing evidence
strength. No arbitrary APK corpus or Android-device differential run was
performed, and no unobserved third-party application crash is claimed.

## Reproduction and primary sources

New runners: `tools/test_wifi_data_contracts.py`,
`tools/test_network_snapshot_contracts.py`,
`tools/test_host_event_loop_contracts.py`,
`tools/test_audio_state_contracts.py`,
`tools/test_surface_geometry_contracts.py`. Native/media runners accept
`--sanitize`; relevant runners accept `--source-ref` for original-source
controls. Java runners use the checked SDK, real core/framework inputs and
JDK21; network also takes the built app dependency JAR. Baseline source ref:
`45a278b4c506978f2a39c4070df00a19e303c908`.

The class/member inventory tools take the SDK, ordered
core-oj/core-libart/core-icu4j/framework/gesher-app boot JARs and
`--candidate-replace gesher-app.jar=<candidate>`. Their 31 parser tests passed.
Hash all inputs before comparing snapshots; the JSON identifies reused inputs.

Primary Android references, all pinned to `android-16.0.0_r4`:

- [Connectivity values](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/) and [LinkPropertiesUtils](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/staticlibs/framework/com/android/net/module/util/LinkPropertiesUtils.java).
- [Wi-Fi values/configuration](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/tags/android-16.0.0_r4/framework/java/android/net/wifi/), with import hashes in the Wi-Fi report.
- [AudioTrack.cpp](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-16.0.0_r4/media/libaudioclient/AudioTrack.cpp), [public AudioTrack](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/java/android/media/AudioTrack.java) and [SoundPool Stream.cpp](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/jni/soundpool/Stream.cpp).
- [Surface.cpp](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/gui/Surface.cpp), [BufferQueueProducer.cpp](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/gui/BufferQueueProducer.cpp) and [ANativeWindow.cpp](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/nativewindow/ANativeWindow.cpp).

Native references: Linux v6.6
[eventpoll.c](https://github.com/torvalds/linux/blob/v6.6/fs/eventpoll.c),
[read_write.c](https://github.com/torvalds/linux/blob/v6.6/fs/read_write.c),
[eventfd.c](https://github.com/torvalds/linux/blob/v6.6/fs/eventfd.c) and
[select.c](https://github.com/torvalds/linux/blob/v6.6/fs/select.c), supplemented
by real Linux syscalls. Linux implementations were read as references rather
than copied into the runtime.
