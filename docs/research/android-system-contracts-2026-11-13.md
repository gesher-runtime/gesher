# Android system contracts, repository map and remaining API gaps

Date: 2026-11-13. Baseline: [`b9a94530c6bf91f7a14913e3ef284dd76bdf28bf`](https://github.com/gesher-runtime/gesher/commit/b9a94530c6bf91f7a14913e3ef284dd76bdf28bf).
Account for this work: **palmoni5**.

Local build/inventory revisions and their published equivalents are preserved in
the [source publication receipt](android-system-source-receipt-2026-11-13.json).
Publication through the authenticated account creates new commit identities;
each complete Git tree was verified identical before advancing the branch.
In particular, local inventory candidate `402dee91` is publicly accessible as
[`a071489b`](https://github.com/gesher-runtime/gesher/commit/a071489b2ec71ab1aa13c762721fe18027db27ec),
and local source `c1fd53a2` with the completed native/Sync QA corrections as
[`b6cd3b3c`](https://github.com/gesher-runtime/gesher/commit/b6cd3b3c27e15b051f770226ead45e519fedd3dd).

The fresh baseline and reconstructed candidate inventories find **910 public SDK
classes absent from the selected boot path**, **548 public/protected member
descriptors absent from classes that are present**, and **502 possible modifier
differences**. These are reproducible structural differences, not counts of
crashing apps, proved defects, or a percentage of Android compatibility.
Selected behavioral repairs address Settings, Sync, Cookie callbacks, keyboard
history, sensor flushes, directory traversal and futex synchronization; Android's
remaining service, hardware and lifecycle contracts are substantially larger.

This report accompanies two compact inventories:

- [SDK class/member differences](android-system-api-gaps-2026-11-13.json): all
  910 class names, all 548 missing members on present classes, all 502 possible
  modifier differences, and all nine present descriptors referring to absent
  boot types. The 7,325 member declarations on absent classes are accounted for
  per class; their individual signatures are in the reproducible full JSONL.
- [Service and JNI source candidates](android-system-service-jni-gaps-2026-11-13.json):
  every inventoried explicit service method, inherited Default candidate,
  direct service registration, Context key candidate, upstream JNI registration
  and generated/attach stand-in row, with source locators and source hashes.

The earlier local checkout became inaccessible before publication. The baseline
inventory, application builds, regression controls and counts here were produced
again from accessible source. Earlier unpublished commits and old pass counts
are not evidence for this reconstructed branch. The preserved
[previous deep audit](android-compatibility-deep-audit-2026-11-13.md) describes
its own scope and remains a separate historical record.

## Scope, provenance and integration boundaries

The baseline contains merged [PR #13](https://github.com/gesher-runtime/gesher/pull/13).
The following open work was inspected to avoid duplicating implementation:

| Pull request | Inspected head | Relevant boundary |
| --- | --- | --- |
| [#14](https://github.com/gesher-runtime/gesher/pull/14) | `6c8ca61c4d7024c63ca6019aed6b3c58d13827a9` | Network builders/callbacks, WebMessagePort, timerfd and codec/surface lifecycle. Cookie, futex and traversal changes must coexist with that implementation and test wiring. |
| [#15](https://github.com/gesher-runtime/gesher/pull/15) | `d2e11ea48580e46cba48156aafa8308476e39813` | Device idle/shutdown accounting. An active sync must explicitly contribute device work; merely binding a background service is insufficient under that policy. |

Independent service, native, Cookie, input and sensor reviews completed. A
non-mutating merge-tree check of source `c1fd53a2`, including their corrections,
together with both inspected PR heads completed without conflicts, producing
tree `ce176eb9b104cad7ad761ab7791c41e49eb82e42`. A mixed futex smoke test against
the prospective PR #14 shim passed 164 assertions. With the complete PR #15
application freshly built, the Sync fixture passed 319 host assertions and a
separate idle-policy probe passed 11 additional checks, including active work
after ten idle timeouts and bounded grace for a future retry. The
[portable probe and reproduction instructions](../../tests/sync/README.md)
load every production class from the fully built combination. These are source,
host and cross-compilation results. The published PR's current-head Actions
checks and description record actual Windows qualification; this report does
not substitute an earlier run or a local host model for that result.

The reference is official Android SDK 36 and the repository's pinned AOSP
[`android-16.0.0_r4`](../../aosp.lock). The local `frameworks/base` checkout
resolves to `45034f0663f960d9ee5fb0a101a4732b71f6e2f4`. The selected boot order
is core-oj, core-libart, core-icu4j, framework, then gesher-app. The scanner's
`installed` field means these input bytes; it does not establish what is
installed on a user's PC.

### Verified platform inputs and fresh application builds

The platform Java inputs came from
[Actions run 43363980751](https://github.com/gesher-runtime/gesher/actions/runs/43363980751),
whose workflow head was `1b6827d2369eb9b6a46381e803940b7b11c85556` and checked-out
source was `0ff811e9acfb457236efdc021fd83a7c36cab6ad`. Download digests were
verified again after recovery:

| Artifact | ID | Archive SHA-256 |
| --- | --- | --- |
| framework-classes | 14009304784 | `85f895adba935980f7255fadc182cb413c8ffec13ee1b72b80622c583c830736` |
| framework | 14010142445 | `ab51bffd74c09837cb004e370a6db65e63c83e5a0b9bf1a717d3726bfdc4ea45` |
| framework-assets | 14009478107 | `51a4a029b977800d296997a8aab090fcb5a05e17184fff1a2e04e47bb1f7e84c` |

The framework class ZIP *inside* its download hashes to
`6bcd1be72efbb6befe0c537eeff87f13a7a0f60af9bbd26b3f2a9c7e98da59f5`; the
installed-format framework JAR hashes to
`3a8fa879c625aa029bedeced3886e58acfe18ed375a64695debb28083e86a957`.
Official SDK36 android.jar hashes to
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
All five baseline and candidate boot-JAR hashes are in the API JSON.

A fresh Git comparison from the artifact source to baseline `b9a94530` found no
changes under `aosp.lock`, `tools/framework`, `tools/build-boot-jars.sh`,
`tools/make-coremod.sh`, `tools/build-framework-res.sh`, `tools/gen`, `config`,
`java`, or `patches`. That supports reuse of these platform compilation inputs;
it does not turn their old build into a new full AOSP build. The baseline and
candidate applications were freshly compiled against them. Baseline
gesher-app.jar SHA-256 is
`9c5758203334261fd8cd2612e296291c3e57a6d5d4883274e1ea12e8f2ea8337`.
Candidate `402dee91927ae5538636b7c498bffd87c67cb585`, including Sync, passed
javac/D8 and location-provider builds; its application JAR SHA-256 is
`106fb138b851f1adbc64e8af119c6d19c7c674b45045e0f07dec239c4e7e389a`.
The later native correction does not change these Java inputs. The final Sync
QA fixture was rebuilt through javac/D8 after adding cancellation-retirement
assertions. The complete graphics fixture build also passed.

Verified toolchain inputs:

| Input | Version | SHA-256 |
| --- | --- | --- |
| Temurin JDK | 21.0.13+9 | `d7f5d5cf865a2902b70ff549b97586a7d3f234e016f041ab447ea740c7d66d1c` |
| LLVM-MinGW UCRT Ubuntu x86-64 archive | 20261006 | `5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4` |
| NDK archive | r27c | `59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc` |
| R8 | 9.5.26 | `3502f087388ce95bbf5254555927f09385dc2636873964d0bd53f3eaf4fb49ad` |
| apksig | 8.13.0 | `c070ed1394629d74641aa0906f60b2ffa1ee77e6366a1f93437f59717b1aeb89` |

## Repository map and review depth

`git ls-tree -r --name-only b9a94530` reports **2,873 tracked files**. By root:
`tests` 1,118; `compat` 507; `docs` 324; `tools` 323; `app` 316; `.github` 83;
`probes` 83; `patches` 73; `cmake` 33; `config` 4; `assets` 1; `java` 1; and
seven root files. These are navigation counts, not executed-test counts.

| Layer | Main source/build entry points | Translation boundary and review focus |
| --- | --- | --- |
| APK loading and startup | [Main](../../app/java/com/gesher/app/Main.java), [ApkPackage](../../app/java/com/gesher/server/ApkPackage.java), [Installer](../../app/java/com/gesher/server/Installer.java), [Zygote](../../app/java/com/gesher/server/Zygote.java), `compat/shell`, `compat/windows` | Manifest/resource/ABI selection, app data and process ownership. Successful DEX loading does not establish component lifecycle behavior. |
| AOSP framework and Java core | [aosp.lock](../../aosp.lock), `tools/framework`, `tools/build-boot-jars.sh`, `app/compat-java`, `patches` | Module selection, generated boot classes and app-side replacements. Class/member inventories cover this boundary structurally. |
| Binder and service dispatch | [Services](../../app/java/com/gesher/server/Services.java), [SystemServer](../../app/java/com/gesher/server/SystemServer.java), [StandIn](../../app/java/com/gesher/server/StandIn.java), `compat/binder` | Binder transport, identity, object death, exceptions and Default dispatch. Names alone do not establish overload or behavioral coverage. |
| Providers and scheduled work | [ContentProviders](../../app/java/com/gesher/server/ContentProviders.java), [SettingsProvider](../../app/java/com/gesher/server/SettingsProvider.java), [SyncManager](../../app/java/com/gesher/server/SyncManager.java), [JobSchedulerService](../../app/java/com/gesher/server/JobSchedulerService.java) | Cursor/Binder transport, durable data, observers, cancellation, binding and device-work accounting. Settings and selected Sync contracts are this round's focus. |
| POSIX/kernel interfaces | `compat/posix/include`, `compat/posix/src`, [posix.cmake](../../compat/posix/posix.cmake) | Windows handles/files/memory/sockets/waits must retain Linux errno, fd, clock and lifetime contracts. Tree walks and futex queues are examined here. |
| ELF and bionic ABI | `compat/elf`, `compat/bionic`, [shim.cc](../../compat/bionic/shim.cc) | Symbol resolution, TLS, callback structures and library ownership. Successful linking is weaker evidence than guest execution. |
| ARM64 translation | `compat/arm64/dll`, [guest_libc.cc](../../compat/arm64/dll/guest_libc.cc), `compat/arm64/arm64_runtime.cc` | Dynarmic execution, calling convention and guest/host structures. Futex timeouts and traversal stat callbacks receive targeted coverage. |
| JNI attachment | [android_runtime.cc](../../compat/framework/android_runtime.cc), [attach_standins.cc](../../compat/framework/attach_standins.cc), `compat/libcore` | Native declarations, registration order, optional-class skipping and later replacement of stand-ins. |
| Graphics/window surfaces | `compat/hwui`, `compat/opengl`, `compat/angle`, `cmake/aosp` | AOSP HWUI/Skia and GLES reach ANGLE/Direct3D. Surfaces, fences and asynchronous callbacks require lifetime and device checks; PR #14 owns overlapping repairs. |
| Input and IME | `compat/hwui/window`, `compat/input`, [WindowInput](../../app/java/com/gesher/app/input/WindowInput.java) | Win32 key state/timestamps become Android InputChannel events. This round repairs physical key history and sided modifiers; TSF composition remains broader. |
| Sensors/location/camera | `compat/devices`, `compat/camera`, [DeviceServices](../../app/java/com/gesher/server/DeviceServices.java), [LocationManagerService](../../app/java/com/gesher/server/LocationManagerService.java) | WinRT/Media Foundation availability, timestamps, permissions and async operations. Sensor flush ordering is tested; physical-device completeness is unproved. |
| Audio/media | `compat/media/audio`, `compat/media/jni`, `compat/media/video`, `compat/media/ndk` | WASAPI, Media Foundation and software codecs must implement Android state/callback/format contracts. Earlier audio and PR #14 codec repairs remain separate. |
| Network/DNS/trust | `compat/net`, `compat/posix/src/socket.cc`, `compat/posix/src/network_dns.cc`, app network services | Winsock, host adapters/proxy/trust stores, Conscrypt and BoringSSL. Class presence, callbacks and packet behavior need different evidence. |
| WebView | `compat/webview`, `app/java/com/gesher/app/webview` | WebView2 navigation, UI affinity, callbacks and cookie/storage semantics. Cookie completion affinity is repaired; WebMessagePort work belongs to PR #14. |
| Build/packaging/QA | `.github/workflows`, `CMakeLists.txt`, `tests/windows`, `tests/native`, `tests/dex-services`, `probes`, `tools/tests` | Host checks, cross-compilation, Windows execution, ARM64 translation and packaging are separate gates. Test DLL inclusion and executable-owned futex state must survive actual linking/packaging. |

This maps the tracked tree and systematically inventories the declared surfaces.
Manual behavioral review concentrates on the named contracts and interactions.
It does not claim every line, hidden Android API, module, hardware feature or
arbitrary APK path has been proved correct.

## Static API and registration findings

The scanners read class-file and DEX definitions, then resolve exact JVM member
descriptors through supplied superclass/interface chains, following their
[DEX](https://source.android.com/docs/core/runtime/dex-format) and
[JVM resolution](https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-5.html#jvms-5.4.3)
models. Constructors are not inherited; unresolved ancestry is not silently
classified as definite absence.

| Measurement | Baseline and candidate result | Meaning |
| --- | ---: | --- |
| SDK class definitions | 6,227 | Official SDK36 reference |
| SDK classes present / absent | 5,317 / 910 | Selected boot union, excluding other providers |
| SDK public/protected declarations | 90,630 | Each declaration counted once |
| Exact descriptor, without flagged modifier difference | 82,255 | Bodies, constants and behavior untested |
| Descriptor absent from present class | 548 | 263 fields, 283 methods, two constructors |
| Declaration on absent class | 7,325 | Separate from present-class member gaps |
| Exact descriptor with possible modifier difference | 502 | 498 new method-final and four declaring-class-final restrictions |
| Present descriptor refers to absent boot type | 9 | Overlapping diagnostic, not another mutually exclusive status |
| Exact rows resolved through inheritance | 773 | Included in the presence counts |
| Duplicate boot class definitions | 35 | First input wins in this model: six core-libart/framework, 29 framework/gesher-app |

Baseline boot union contains 45,397 classes. Every one of the **90,630 expanded
member-result rows is byte-for-byte identical** between baseline and candidate
`402dee91`; both full JSONL files hash to
`de6c2ed215f9bb52b8b972d77c1f229ad0daace4bb65a2e57f2ab3f99ebc3ec6`.
This supports the narrow conclusion that the behavioral repairs did not change
this structural SDK comparison. It does not prove identical behavior.

Absent class families: connectivity 264; other framework 212; HealthConnect 207;
AdServices 154; Bluetooth 41; NFC 21; SDK sandbox nine; AndroidX annotations two.
Present-class missing members: connectivity 281, Bluetooth 142, NFC 59, SDK
sandbox 26, HealthConnect 24, other framework 16. These labels guide module and
provider investigation; they do not mean every app requires every family.

The nine referenced-type rows include AppSearch `GenericDocument`, media
`Session2Token` and `IkeTunnelConnectionParams`: a present descriptor can still
refer to a type absent from this boot set. Conversely, an APK, module, shared
library or desugaring provider may supply an absent type. Neither observation
alone proves a particular app crashes.

| Source inventory | Baseline and candidate summary | Qualification |
| --- | ---: | --- |
| Default/Stub classes | 70 | 67 interfaces available in supplied framework class ZIP |
| Explicit service methods | 1,058 | 874 nontrivial bodies remain semantically unverified |
| Explicit empty collections / default answers / noops / unsupported paths | 12 / 80 / 78 / 14 | Source classification, requiring per-contract review |
| Inherited Default candidates | 2,767 | Interface method names, not full overload/type equivalence |
| Direct registrations / Context key candidates | 51 / 197 | Local managers and alternate Binder keys require review |
| Upstream JNI registration rows | 174 | 50 textual references, four reviewed aliases, 120 without a local textual reference |
| Attach/generated stand-ins | 523 | Some are replaced later; presence does not prove an active fallback |

The [service/JNI JSON](android-system-service-jni-gaps-2026-11-13.json) contains
candidate `402dee91` source rows and hashes, with the baseline receipt and
summary equality preserved. Counts are unchanged despite changed method bodies.
The current Settings family is explicitly partial: durable rows are implemented,
while generations, per-user state and restore metadata remain absent. All source
locators belong to their recorded source revision, not necessarily a later head.

## Repaired contracts and Windows translation

### Settings: durable rows and ordinary provider transport

[SettingsProvider](../../app/java/com/gesher/server/SettingsProvider.java) now
uses the same rows for `call()` and public CRUD.
[SettingsStore](../../app/java/com/gesher/server/SettingsStore.java) records
nullable UTF-16 values and row identities in a versioned file, using an owned
temporary file, sync and replacement, with in-memory rollback on failure. Null
remains a row value; deletion removes the row. Projections, supported name
selectors, MIME types, change counts and errors are explicit. Notifications run
after publication, outside the provider lock, with calling identity restored.
The pinned references are
[AOSP SettingsProvider](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java)
and [SettingsState](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/packages/SettingsProvider/src/com/android/providers/settings/SettingsState.java),
including the 32,768-character bound and nullable values.

A real [ContextImpl](../../app/java/android/app/GesherSettingsContext.java) is
prepared on the main Looper before normal provider attachment. The
[remote fixture](../../tests/dex-services/gesher/services/SettingsProviderRemoteTest.java)
requires an ordinary-UID client BinderProxy, real CursorWindow transport and
reentrant observers against the system-UID server. This is a separate Windows
gate because same-process host calls cannot prove ContentProvider.Transport.
The fixture retains the existing write-policy configuration; complete Android
permission policy, generation arrays, per-user state, validators, moved settings
and restore metadata are outside this repair.

### Sync: admission, clocks, results and cancellation

[SyncManager](../../app/java/com/gesher/server/SyncManager.java) now separates
structured endpoint/work identity, queued work and active bindings. Nonparallel
adapters serialize conflicting accounts; parallel adapters may run independent
accounts, while same-account reruns remain queued. Elapsed time drives start,
heartbeat, timeout and retry deadlines. Wall-clock `delayUntil` is converted to
an elapsed deadline once, preventing later wall changes from moving that wait.
Result handling retains application extras while applying retry/upload rules,
backoff and independent empty-extras full-sync requests. The comparison reference
is pinned [AOSP SyncManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/content/SyncManager.java).

Binding, connected/start, completion and cancellation callbacks run on a serial
lifecycle executor. External callbacks occur outside the registry monitor.
Successful binding owns one release; a callback during binding is deferred until
ownership is known, and cancelled late connections cannot revive retired work.
Binder calling identity is cleared/restored for system-owned bind/unbind calls.
A `DeviceWork.Source` advertises active sync as due now, preserving work under
PR #15's idle policy independently of generic background-service grace.

The [contract fixture](../../tests/dex-services/gesher/services/SyncManagerContractTest.java)
uses actual production admission/result code, controlled clocks and reentrant
cancellation. Its Windows mode adds generated AIDL/Parcel dispatch and real
AbstractThreadedSyncAdapter workers/provider clients; the host mode does not
execute that additional worker path. Independent services QA passed 319 host
checks and strengthened device-work accounting during cancellation retirement.
Persisted Android retry/history,
periodic-key migration, JobScheduler/network policy and quotas remain separate.

#### Second PR review: settings changes during queued sync work

Review of published head `0dd231339b24363368a531a26ee3760f72f294ea` found two
additional admission defects. A retry could bind after master sync, its authority's
automatic sync, or its syncable state had been disabled during backoff. Manual
requests also failed to acquire Android's ignore-backoff flag, so an explicit
request could remain behind an earlier automatic retry.

Admission now reads current settings before binding and again before starting an
adapter whose connection arrived later. Rejected work releases its binding and
does not retry. Manual flags are normalized on the manager's copy of the request;
the initial manual attempt bypasses backoff, while a failed attempt still loses
ignore-backoff and waits normally. Explicit `syncable=0` remains authoritative.

The final shared fixture passes **346 host checks**. Its new public-request cases
use the real package resolver and assert that the constructed adapter is found.
Three compiling controls fail their intended behavioral assertions: the published
head starts a retry after master sync is disabled; removing only manual-flag
normalization blocks the manual request; removing only the connected-adapter
policy check starts work after policy changes during binding. The complete app
and service fixture were freshly compiled through javac/D8. These host and build
results require fresh Windows CI before merge; the earlier 319-check receipt is
historical evidence for its recorded source.

### Cookie callbacks: return to the calling Looper

[GesherCookieManager](../../app/java/com/gesher/app/webview/GesherCookieManager.java)
captures a Handler before validation or native effects and posts every nonnull
completion through it. WebView2's worker-thread and immediate-completion paths
therefore share the caller-Looper contract. Nonnull callbacks without a Looper
are rejected before native work; null callbacks require no Looper. Pinned
[AOSP CookieManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/webkit/CookieManager.java)
specifies this affinity for set/remove operations.

The host fixture uses actual framework Handler/Looper/MessageQueue with a pthread
native wait/wake adapter and explicit WebView2/tracing boundaries. Windows must
load the native bridge and complete a real cookie operation in an isolated
WebView2 profile. Cookie parsing, persistence/flush and file-scheme parity remain
unqualified. Deletion booleans still indicate native success rather than whether
cookies existed; that distinct contract is not claimed as repaired.

### Keyboard: physical key identity and sided modifiers

Win32 collection now retains sided Shift/Ctrl/Alt/Meta and lock state through
[input_modifiers.h](../../compat/hwui/window/input_modifiers.h), the window
producer and IME path. [KeyHistory](../../compat/input/key_history.h) retains
original keyCode, downTime and repeat state until physical-key release, with
bounded fallback indexing and focus-loss reset. The pinned reference is
[KeyboardInputMapper](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/services/inputflinger/reader/mapper/KeyboardInputMapper.cpp).

Independent QA strengthened Windows [InputTest](../../tests/dex-graphics/gesher/InputTest.java)
to wait for delivery of the first chord key before posting the second. Otherwise
a stalled message loop and production's message-age cap could give two old
synthetic messages the same timestamp. Host extraction checks replace Win32
state; real InputChannel, keyboard layout, IME and physical-device behavior
still require Windows qualification.

### Sensors: preserve accepted flush completions

[SensorEventBuffer](../../compat/devices/sensor_event_buffer.h) bounds storage
while protecting accepted completion markers from subsequent sample eviction.
A multi-sensor flush is admitted completely or rejected before mutation; full
control capacity produces a native failure that the framework returns as false.
The sampler mutex serializes samples, subscription changes and flush snapshots;
dispatch occurs after releasing locks. Pinned
[SensorManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/hardware/SensorManager.java)
and [SystemSensorManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/hardware/SystemSensorManager.java)
define the completion and all-active-sensors behavior.

Windows [SensorSteps](../../tests/apps/devices/java/com/gesher/devices/SensorSteps.java)
blocks callback consumption, accepts 128 paired flush requests, rejects overflow
and requires 256 completions. Independent QA added a callback-Looper barrier
and an extra-marker check so partial admission of a rejected request cannot
pass unnoticed. Existing bounded sample-data loss remains; this is not unlimited
FIFO retention or physical sensor parity.

### Traversal: Android walk semantics and guest callback ABI

The [common walk](../../compat/posix/src/ftw.cc) covers positive descriptor-budget
validation, callback returns, all sixteen POSIX flag combinations, ancestor
cycles, directory-read failures, mount boundaries and physical traversal's
callback cwd. It enumerates one directory, snapshots names, then descends and
restores the original cwd using an owned fd. Completed aliases can be revisited;
only ancestors suppress cycles. Root-link, failed-follow, slash and basename
details follow pinned Android
[ftw.cpp](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/ftw.cpp)
and [fts.c](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/fts.c),
whose behavior differs from assumptions based only on glibc.

Independent QA found that Gesher's drive-stripping `getcwd()` could misresolve
access paths after a Windows cross-volume `FTW_CHDIR`. The walk now restores
its owned initial directory before resolving an access path or callback parent.
Direct second-volume roots and links to them exercise preorder/postorder,
multiple siblings, callback directory identity and initial-directory restoration.

The bionic shim now exposes `ftw`/`ftw64` alongside `nftw`/`nftw64`, preserves
nested callback state and converts the x86-64 stat callback payload to the ARM64
guest structure. The mount fixture requires an actual second filesystem, not a
second folder on the same volume. Native Windows, x86-64 ELF and ARM64 ELF gates
must run the real routes. Concurrent rename/replacement identity, global cwd
interactions, complete FTS and all allocation-failure paths remain unqualified.
The [native evidence note](native-traversal-futex-contracts-2026-11-13.md) records
detailed semantics and source receipts.

### Futex: queue selection, lifetime and one executable owner

The [futex engine](../../compat/posix/src/syscall.cc) implements WAIT, WAKE,
WAIT_BITSET, WAKE_BITSET, REQUEUE and CMP_REQUEUE with explicit waiter queues,
address/private-mode/mask selection and actual selected/moved counts. Queue
removal and wake state share synchronization so a waker cannot retain a pointer
to an expired stack waiter. Guest timeout validation precedes narrowing; waits
retain their original deadlines through slices and spurious wakeups. Realtime
clock changes are rechecked within a bounded interval, not instantaneously.

Independently linked PE modules delegate to the executable's exported
`gesher_futex_owner_v1`, failing with ENOSYS when it is absent. The linker must
force the private, unexported `gesher_futex_owner_anchor_v1`: forcing only the
public owner can resolve it from an earlier DLL import library, leaving a
successfully linked executable without its own owner. A dedicated module-first
link probe checks that regression. Host/guest routes, a separate DLL and both
guest ABIs require Windows execution. The [native note](native-traversal-futex-contracts-2026-11-13.md)
separates focused link evidence from real execution and documents unsupported
PI, robust owner death, WAKE_OP, Linux signal interruption and cross-process or
different-address shared aliases.

## Fresh validation and limits

These counts refer to reconstructed code. A host assertion is not an Android
CTS case or a supported-app count.

| Gate | Fresh result available | Boundary |
| --- | --- | --- |
| Settings | 61 independently rerun host checks; six baseline controls compile then fail intended query/CRUD/call/persistence/limit/observer expectations | Actual provider/store with declared host boundary; ordinary-UID remote transport needs Windows |
| Sync | 319 independently rerun host checks; four baseline controls compile then fail intended assertions | Production admission/clocks/results/cancellation; Windows adds AIDL and actual adapter workers |
| Sync with PR #15 | Full combined application and service fixtures build; 319 Sync checks plus 11 idle-policy checks pass | Actual merged service/activity registry and DeviceWork policy, with declared host JNI boundaries |
| Cookie | 86 asynchronous + 86 immediate + 90 unavailable-backend checks | Actual framework Looper; controlled WebView2 completion. Independently rerun |
| Keyboard | 50 ASan/UBSan checks; baseline compiles and fails 38 expectations | Extracted production path with controlled Win32 state, independently rerun |
| Sensors | 920 ASan/UBSan checks; baseline compiles and fails eight checks | Actual buffer/sampler with controlled source, independently rerun |
| Traversal | 485 ASan/UBSan walk assertions plus 34 adapter assertions | Independently rerun; real host filesystem and second filesystem |
| Futex | 396 ASan/UBSan, 316 ThreadSanitizer, 232 real-Linux-reference assertions | Independently rerun supported public/mixed-route/concurrency contracts |
| Futex with PR #14 | 164 mixed guest/host ASan/UBSan assertions | Prospective merged bionic syscall adapter; Windows transport remains separate |
| Futex negative control | Baseline production compiles, then fails ten of 18 validation assertions | Fixture sensitivity without relying on compilation failure |
| Native cross-compilation | Changed production units/native fixtures compile under pinned LLVM-MinGW; both guest ABIs compile under NDK r27c API26 | Compile/link evidence, distinct from Windows execution |
| Actual POSIX CMake/PE link | Complete POSIX library and native traversal/futex/module/owner targets build with actual product flags/target declarations; module import library precedes POSIX archive | EXE exports owner without importing it; private anchor is not exported. Replaying with the old public-symbol option reproduces the missing EXE owner |
| Java builds | Baseline/candidate application, service and graphics fixtures pass javac/D8; location provider passes; final Sync QA fixture rebuild passes | Verified reused platform inputs; production app bytes remain those recorded for `402dee91` |
| Inventory tooling | 52 existing compatibility/class/member/service scanner unit tests pass | Parser/classification checks, not runtime compatibility |
| CI configuration | Changed PowerShell scripts parse; four YAML files and 90 plain Bash blocks pass syntax checks | Syntax only, not workflow execution |
| Published-head Windows CI | Read the PR's current-head Actions result and linked evidence | Real Binder/CursorWindow, WebView2, Win32 input/sensors, separate PE ownership, second volume and ARM64 callbacks |

Local ASan/UBSan used `ASAN_OPTIONS=detect_leaks=0` because this environment runs
under ptrace and LeakSanitizer refuses that mode. Committed runners do not carry
that local exemption. ThreadSanitizer results apply to the exercised host
concurrency groups, not every Windows callback race. Baseline controls must
compile and reach their intended assertions; unrelated compile/link failures
or unavailable backends do not qualify.

Independent Cookie/input/sensor QA found no new production blocker and improved
the two Windows fixtures described above. Its Cookie baseline controls compile:
asynchronous completion lacks the required callback, while immediate completion
raises baseline NullPointerException for a nonnull callback without a Looper.
The new code explicitly checks that contract before effects. Final PR evidence
must identify the tested head and its own native/framework/ARM64 workflow runs;
an intermediate host pass or another PR's green run is insufficient.

Independent integration review found no additional blocker in CMake, artifact
packaging, failure propagation or the compact inventories. Keyboard track33
and sensor track58 run in the full Windows suite. Track98's Settings, Sync,
Cookie, traversal and futex routes run in both the full and framework-contract
suites. Test executables and the test DLL are excluded from the installer by
its explicit file list.

## Remaining priorities and source evidence

The list separates a reviewed mismatch from an area requiring additional
behavioral work. It does not imply every app using that area crashes.

| Priority | Finding or requirement | Evidence and next qualification |
| --- | --- | --- |
| 1 | Origin-specific WebStorage deletion clears all data | [GesherWebStorage.deleteOrigin](../../app/java/com/gesher/app/webview/GesherWebStorage.java) calls `deleteAllData()`. Implement and verify origin-scoped WebView2 deletion before claiming the Android contract. |
| 1 | Provider reference lifecycle is incomplete | [ActivityManagerService.removeContentProvider](../../app/java/com/gesher/server/ActivityManagerService.java) is empty; [ContentProviders.refContentProvider](../../app/java/com/gesher/server/ContentProviders.java) returns true and counts clients by process. Stable/unstable changes, release and client death need one coherent lifecycle. |
| 1 | Host pthread CPU clock ignores the target thread | [pthread_getcpuclockid](../../compat/posix/src/pthread.cc) ignores `thread` and returns CLOCK_THREAD_CPUTIME_ID. This is the host POSIX entry point, distinct from existing guest clock-target repairs. |
| 1 | Default service methods need contract-by-contract review | [Complete service candidates](android-system-service-jni-gaps-2026-11-13.json) include 2,767 inherited names and explicit noops/defaults. Resolve overloads and local service injection; prioritize framework paths expecting nonnull results or eventual callbacks. |
| 1 | Missing module/member providers remain | [Complete API differences](android-system-api-gaps-2026-11-13.json) identify exact present-class missing descriptors and all absent classes. Establish actual module/APK/shared-library providers and guarded paths before adding blanket stubs. |
| 2 | Settings remains partial | [SettingsProvider](../../app/java/com/gesher/server/SettingsProvider.java) documents absent generations, per-user state and restore metadata. Validators, moved tables and modern call/query variants need their own pinned contracts. Durability does not establish those behaviors. |
| 2 | Sync is not the full Android scheduler | [SyncManager](../../app/java/com/gesher/server/SyncManager.java) retains property-backed settings and executor scheduling; persisted periodic keys use delimiter identity plus a hash of marshalled extras. Persistent retry/history, migration/collision handling, JobScheduler/network policy and quotas remain separate work. |
| 2 | Futex sharing/operations are limited | [Native evidence](native-traversal-futex-contracts-2026-11-13.md) explicitly excludes PI, robust owner death, WAKE_OP, Linux signal interruption, cross-process and shared-file aliases. Six supported command fixtures cannot resolve those omissions. |
| 2 | Tree walking remains path based | [ftw.cc](../../compat/posix/src/ftw.cc) snapshots names then queries paths. Concurrent rename/replacement identity, process-wide cwd and complete FTS/allocation-failure behavior remain unqualified. |
| 2 | Dynamic-library flags/scopes/ownership need wider tests | [dlfcn.cc](../../compat/posix/src/dlfcn.cc) separates built-in, PE and ELF routes. PE dlclose calls FreeLibrary; ELF dlclose delegates to loader operations. Test repeated opens, scopes and unload dependencies separately; labeling all closes noops would be inaccurate. |
| 2 | Cookie/storage semantics remain broader than affinity | [GesherCookieManager](../../app/java/com/gesher/app/webview/GesherCookieManager.java) still needs deletion-boolean, parser, persistence and file-scheme qualification. |
| 2 | Location features/timestamps need explicit qualification | [LocationManagerService](../../app/java/com/gesher/server/LocationManagerService.java) documents absent geofencing/GNSS measurements and logs unmonitored geofence requests. [WinRT location](../../compat/devices/location_win.cc) converts wall timestamps; cached-age/elapsed-time and cancellation/permission behavior need device-level tests. |
| 2 | Native receiver lifetimes and media/device callbacks need continued review | `compat/media/jni`, camera and surface/fence paths retain native references across async work. PR #14 owns particular codec/surface fixes; remaining close/callback/error/cancellation methods need separate fixtures. |
| 3 | Hardware/Windows variants need execution evidence | Real cameras, microphones, sensors, keyboards/IME layouts, multi-monitor setups, GPU drivers and host permissions cannot be proved by fake sources or cross-compilation. Unavailable capabilities and callbacks must still obey coherent Android contracts. |

## Reproduction and compact inventory format

The existing scanners produce the authoritative expanded outputs. Use a clean
worktree to select the intended source revision without disturbing active work:

```sh
git worktree add --detach ../gesher-system-baseline b9a94530c6bf91f7a14913e3ef284dd76bdf28bf
cd ../gesher-system-baseline

# Verified inputs, arranged as in the restoration receipt.
AUDIT_DEPS=/path/to/verified/deps
export PATH="$AUDIT_DEPS/jdk/jdk-21.0.13+9/bin:$PATH"
AOSP_DIR="$AUDIT_DEPS/aosp" R8_JAR="$AUDIT_DEPS/r8.jar" \
  APKSIG_JAR="$AUDIT_DEPS/apksig.jar" \
  bash app/build.sh out/system-baseline \
    --framework-classes "$AUDIT_DEPS/framework-classes/classes" \
    --core-classes "$AUDIT_DEPS/framework-classes/coremod"

python3 tools/java_api_inventory.py \
  --sdk-jar "$AUDIT_DEPS/sdk/android-36/android.jar" --sdk-api-level 36 \
  --boot-jar "$AUDIT_DEPS/framework-assets/core-oj.jar" \
  --boot-jar "$AUDIT_DEPS/framework-assets/core-libart.jar" \
  --boot-jar "$AUDIT_DEPS/framework-assets/core-icu4j.jar" \
  --boot-jar "$AUDIT_DEPS/framework/framework.jar" \
  --boot-jar out/system-baseline/gesher-app.jar \
  --outdir out/system-baseline/classes

python3 tools/java_member_inventory.py \
  --sdk-jar "$AUDIT_DEPS/sdk/android-36/android.jar" --sdk-api-level 36 \
  --boot-jar "$AUDIT_DEPS/framework-assets/core-oj.jar" \
  --boot-jar "$AUDIT_DEPS/framework-assets/core-libart.jar" \
  --boot-jar "$AUDIT_DEPS/framework-assets/core-icu4j.jar" \
  --boot-jar "$AUDIT_DEPS/framework/framework.jar" \
  --boot-jar out/system-baseline/gesher-app.jar \
  --outdir out/system-baseline/members

python3 tools/audit_services_jni.py \
  --framework-jar "$AUDIT_DEPS/framework-classes/framework-classes.zip" \
  --aosp-dir "$AUDIT_DEPS/aosp" \
  --out out/system-baseline/services-jni.json
```

`audit_services_jni.py` selects the checkout containing its script as source
root; changing only the shell's cwd does not select another revision. For a
candidate comparison, rebuild its app, retain the other four inputs and boot
order, then use each API scanner's
`--candidate-replace gesher-app.jar=/path/to/candidate/gesher-app.jar`.
Run the source audit from that candidate's own script. Current reports identify
source `402dee91`; later source edits do not silently inherit this evidence.

The compact API JSON groups missing classes by family and members by class.
Column definitions describe each array, and full `installed_resolution` objects
preserve lookup details. It includes every present-class gap and every absent
referenced-type row; ordinary exact-present rows are summarized. The source
JSONL contains all 90,630 rows and can be regenerated above. The compact service
JSON groups repeated source/class/status fields and retains every one of the
1,058 explicit methods, 2,767 inherited candidates, 174 registrations and 523
stand-ins. Temporary absolute prefixes become artifact-relative paths or
`AOSP/android-16.0.0_r4/...`; source hashes retain the scanner's universal-newline
encoding. Both files record input/report digests and generator hashes.

These are complete candidate lists **within their declared scanning scope**.
They do not inventory every private Android API, prove JNI reachability,
replace CTS, or establish compatibility with every application and Windows
configuration. The next repair should begin with a specific Android contract,
its call path and an independent regression that actually reaches the behavior.
