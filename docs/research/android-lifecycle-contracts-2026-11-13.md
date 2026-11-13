# Android lifecycle compatibility: map, API inventory and repairs

Baseline: main `6bfc980c335a8b8ae1a9271dc5da43078561b516`; platform:
`android-16.0.0_r4`. Three research agents examined Java/services, native
runtime, and media/devices. Separate implementers repaired selected contracts;
three independent reviewers checked the integrated changes. The root review
also followed WebMessagePort ownership through the WebView2 boundary. Commits
and the pull request use `palmoni5`.

This is a set of verified repairs and an explicit remaining work queue, not a
certification that every Android application runs. Compatibility includes exact
descriptors, class loading, Binder/JNI registration, errors, asynchronous
ordering, ownership, permissions and hardware. Source presence and host test
success establish only parts of those contracts.

The [machine-readable inventory](android-lifecycle-contracts-2026-11-13.json)
lists all 911 absent public SDK boot classes, all 584 missing declarations on
present classes, all 502 modifier-review rows, nine descriptor-dependency rows,
the nine restored declarations, service/JNI review queues and input hashes.
These categories are not counts of demonstrated application crashes.

## Open PR coordination

[PR #13](https://github.com/gesher-runtime/gesher/pull/13), reviewed through
`45c1c1d8003d81905cc674a4224f3953dbd7f51f`, owns Wi-Fi value types,
LinkProperties/RouteInfo/LinkAddress, event-loop fairness/writev/select,
AudioTrack state, SoundPool auto-pause, and surface geometry/damage. This branch
does not import or duplicate those changes. Workflow additions use separate
steps and track 97; #13 uses track 96. Merge prediction was checked against
that head. Later changes require a fresh check.

This inventory compares the isolated candidate with main. Its 584 missing
declarations must not be compared with #13's 548 as measurements of the same
candidate. A combined tree requires a separate build and measurement.

A disposable merge of the earlier integrated candidate and #13 produced tree
`26338f22de51b358bf2a3922f1760fd0078f1784`. Independent QA on that tree passed
timerfd (77), its real Linux reference (56), input callbacks (55), #13 geometry
(158), AudioTrack state (162) and SoundPool state (10), plus a fresh combined
app javac/D8 build. This receipt precedes the final Web ownership follow-up;
the final production merge also predicts cleanly. These results remain separate
from the isolated API counts and do not claim combined Windows execution.

## Repository map

The baseline has **2,847 tracked files**: `tests` 1,104, `compat` 507, `docs`
321, `tools` 317, `app` 313, `.github` 83, `probes` 83, `patches` 73, `cmake`
33, seven root files, `config` four, and one each in `assets` and `java`.
Java source counts are 83 in `app/java/android`, 86 in `com/gesher/server`,
52 in `com/gesher/app`, four in `com/gesher/net`, 46 in `com/google`, and one
in `org`. These are navigation counts, not semantic coverage claims.

| Layer | Main paths | Boundary examined |
| --- | --- | --- |
| Platform/build | `aosp.lock`, `patches`, `cmake/aosp`, `tools/framework` | Pinned sources, generated Binder defaults, boot artifacts, native link inputs. |
| Java app host | `app/java/com/gesher/app` | APK/bootstrap, class loading, resources, process/activity/window lifetime. |
| System services | `app/java/com/gesher/server` | Package/activity/content/account/sync/alarm/job/device services, permissions, listeners and Binder identity. |
| Module substitutes | `app/java/android` | Network, Bluetooth, NFC and other absent module classes; complete signatures and value semantics. |
| Google-facing substitutes | `app/java/com/google` | Selected task/auth/location/messaging contracts; embedded SDK versions remain separately qualified. |
| POSIX host | `compat/posix` (156 files) | Windows descriptors, paths, memory, readiness, clocks, sockets and synchronization. |
| Android libc/NDK | `compat/bionic` (41 files) | LP64 Android over LLP64 Windows, errno, libc imports, varargs, callbacks, syscalls. |
| ELF/translation | `compat/elf` (21), `compat/arm64` (14) | Relocation/TLS, library lifetime, x64 calls and translated ARM64 entry points. |
| Framework JNI | `compat/framework`, `compat/libcore` | Registration, shared memory, local sockets, attachment and native ownership. |
| Graphics/desktop | `compat/hwui`, `compat/opengl`, `compat/windows` | HWUI/Skia through ANGLE/Direct3D, surfaces, buffers, fences and desktop integration. |
| Media | `compat/media` | AOSP extraction, Media Foundation, WASAPI, surfaces, workers and callbacks. |
| Devices | `compat/devices`, `compat/camera` | Windows camera/location/sensors, timestamps, registrations and hardware absence. |
| Input/text | `compat/input` | Win32 key/touch/pen state, Android channels, Windows text/IME. |
| WebView | `app/java/com/gesher/app/webview`, `compat/webview` | WebView2 translation, message ownership, navigation and disposal. |
| Verification | `tests`, `probes`, `tools/ci`, `.github/workflows` | SDK callers, Linux references, declared host adapters, Windows ART/native execution and artifact identity. |

## Fresh API measurements

Reference: official SDK36 `platform-36_r02.zip`. Ordered boot inputs:
`core-oj.jar`, `core-libart.jar`, `core-icu4j.jar`, `framework.jar`,
`gesher-app.jar`. Verified core/framework binaries and compile classes come
from [run 43302471272](https://github.com/gesher-runtime/gesher/actions/runs/43302471272).
Baseline and candidate app JARs were separately rebuilt with javac and D8 from
their own sources. This is a fresh comparison with reused platform inputs,
not a full platform rebuild performed locally.

| Measurement | Main baseline | Candidate |
| --- | ---: | ---: |
| SDK class definitions | 6,227 | 6,227 |
| SDK classes present on chosen boot path | 5,315 | 5,316 |
| SDK classes absent from chosen boot path | 912 | 911 |
| Public/protected declarations compared | 90,630 | 90,630 |
| Exact descriptor without modifier concern | 82,193 | 82,202 |
| Missing declaration on a present class | 584 | 584 |
| Declaration whose class is absent | 7,351 | 7,342 |
| Possible modifier mismatch | 502 | 502 |
| Present descriptor referring to absent boot types | 9 | 9 |

`EthernetNetworkSpecifier` supplies the public class and nine declarations:
constructor, `CREATOR`, interface-name getter, matching, equality/hash/string
and Parcelable methods. The complete hidden `TestNetworkSpecifier` is imported
for legacy request conversion. Other repairs change behavior behind existing
signatures, so do not reduce the missing-member count.

Absent class families: connectivity 265, other Android framework 212, Health
Connect 207, AdServices 154, Bluetooth 41, NFC 21, SDK sandbox nine, AndroidX
annotations two. Optional module/APK providers outside this boot set are not
evaluated.

| Present class with missing SDK declarations | Rows |
| --- | ---: |
| `WifiManager` | 171 |
| `BluetoothAdapter` | 69 |
| `BluetoothDevice` | 56 |
| `ScanResult` | 44 |
| `WifiInfo` | 38 |
| `WifiConfiguration` | 35 |
| `NfcAdapter` | 33 |
| `SdkSandboxManager`, `CardEmulation` | 26 each |
| `TetheringManager` | 25 |
| `HealthConnectManager` | 24 |
| `BluetoothHeadset` | 13 |
| `NetworkStatsManager`, `MediaStore` | 8 each |
| `BluetoothManager` | 4 |
| `ConnectivityManager` | 3 |
| `WifiManager.WifiLock` | 1 |

The nine dependency rows reference `GenericDocument`, `Session2Token` or
`IkeTunnelConnectionParams`. Modifier concerns comprise 498 method-final
differences and four class-final restrictions; context determines linkage
impact. They are not 502 demonstrated failures. The parser resolves inherited
descriptors and selects the first definition of 35 duplicate boot classes.
It does not prove real loader order, native registration, implementation bodies,
feature availability, permissions or exception equivalence.

## Service and JNI queues

The fresh source scan against the verified compile JAR finds 70 service classes,
67 resolved interfaces and 1,058 explicit methods: 874 implementations not
behaviorally assessed by this scan, 12 empty collections, 80 default answers,
78 no-ops and 14 unsupported paths. It records 2,767 name-based inherited-default
candidates, 51 direct service registrations and 197 Context key candidates.
Name/arity heuristics require overload/dispatch review before a missing
implementation is established.

Of 174 upstream JNI registration rows, 50 have direct local references, four
alternate references and 120 no direct reference in the selected surface.
There are 523 stand-in method entries. Build selection, alternate registration
and replacement of fallback tables matter: these totals do not count proven
active missing bindings. The JSON retains source anchors and methodology limits.

This pass does not relabel the older [native export lookup inventory](systematic-java-native-api-2026-11-05.md)
as a new result. A fresh full NDK export inventory requires the exact Windows
runtime, verified NDK stubs and both guest lookup runs. The present native
evidence covers the selected timer/media contracts and compilation paths;
symbol presence alone would still not establish ABI or behavior parity.

## Repairs

### Network request values and callbacks

Invalid capability mutations now log/ignore instead of aliasing a valid Java
shift bit. Invalid transport mutations throw, and invalid queries return false.
Specifier validation follows pinned Android transport combinations and its
observable mutation-before-invariant-check order. Legacy strings clear null/
empty values, construct telephony specifiers for integer IDs, and use Test or
Ethernet specifiers for other names. Requests reject MatchAll. Enterprise
capability without explicit IDs reports Android's default ID 1. Complete AOSP
value imports retain licenses and [source receipts](aosp-network-builder-values-2026-11-13.json).

Callback registration now uses object identity. Distinct callbacks with equal
`equals/hashCode` cannot replace each other. Cleanup after successful
registration is idempotent, including after timeout; never-registered cleanup
still throws. Timeout retirement claims its exact record atomically, protecting
later reuse. Old events/listener removals retain their original registration.
Remote removal and application unavailable callbacks execute outside the
registry lock. The [detailed network report](network-request-lifecycle-contracts-2026-11-13.md)
records upstream comparisons and remaining boundaries. No Windows enterprise
network or physical interface-selection backend is invented.

### WebMessagePort

Local transfer moves an endpoint to a fresh wrapper, detaches the old one,
rewires its peer and carries queued data. Complete transfer arrays are validated
before mutation: duplicates, source transfer, closed/transferred and started
ports are rejected. Even null callback registration starts a port. Both-end
transfer and retransfer of unstarted received ports preserve their connection.

Handler delivery executes callbacks outside the shared lock. Page transfer
creates all page ports before flushing queued messages, including binary data
and nested transfers. Independent QA additionally found that discarded messages
and page disposal must retire transferred endpoints owned by those messages;
the final ownership follow-up and regressions are included in this branch.

Retirement walks nested/cyclic queued ownership iteratively, preserving ordinary
peers and already-delivered endpoints. Recipient-self transfers consume their
ownership and discard the message, following the HTML posting algorithm.
Destroy/navigation clear and retire page registrations. Page-send admission
checks the exact live destination, so an older flush snapshot cannot register
new endpoints for a closed channel. Native backend emission runs outside the
endpoint graph lock and enqueues work onto the WebView thread.

The public caller compiles against SDK alone; only pair construction uses
reflection. Separate boundary/ownership fixtures declare scheduler/transport
seams. Full JavaScript/WebView2 behavior is not inferred from them. In
particular, page-script targetOrigin rejection and absent JS destinations lack
an acknowledgement protocol that disposes Java-registered transfer endpoints.
That browser-side rejection boundary remains separate work.

### timerfd and Windows timers

Typed shared timer ownership replaces callback lookup by the original integer
fd. Aliases retain the open description; reused descriptors cannot receive old
timer callbacks. Final close disables and joins threadpool callbacks before
freeing context. Settime serializes changes, joins callbacks without holding
their state mutex, and resets unread counts/readiness only on success. Invalid
requests preserve previous settings.

Checked wide arithmetic counts delayed periodic expirations and supports
aliased new/old specification pointers. Timer readiness is read-only. Relative
realtime uses elapsed monotonic time; absolute realtime uses FILETIME. Thin
LP64 adapters and ARM64 syscall numbers 85/86/87 reach the same core.
Absolute realtime `TFD_TIMER_CANCEL_ON_SET` explicitly returns `EOPNOTSUPP`
without changing the timer: Linux clock-change `ECANCELED` behavior is not
implemented. Linux-ignored flag combinations remain accepted. Suspend,
hibernation and complete `CLOCK_BOOTTIME` parity remain unqualified.

### Input surfaces and MediaCodec

InputSurface retains shared state and copied callback captures through
invocation. Replacement/abandonment serialize with admitted callbacks;
abandonment permanently prevents new admission. Self-replacement/abandonment
remain valid. QueueSurface releases its queue mutex before invoking listeners,
and Codec/Recorder abandon surfaces outside consumer data locks. Independent
QA checked this ordering for lock cycles.

Every relevant Codec JNI entry retains a shared receiver. Release detaches the
Java holder under a short context lock and retires work outside it. A lifecycle
mutex serializes worker creation/join/reset, while state/generation changes
wake pending input/output waits. Old dequeue/flush operations cannot consume or
reset a restarted codec's buffers. The unsynchronized `thread.joinable()` flush
predicate is removed. AOSP MediaCodec loads `media_jni` itself and permits a
synchronous constructor without a current/main Looper; its public fixture needs
no fabricated Context. The standalone ART entrypoint also loads `android_runtime`
before calling MediaCodec: its Java dequeue methods use `Trace`, whose natives
are registered by that library. Normal app startup already performs this load.
Native callbacks enqueue Handler messages rather than invoking application
callbacks inline on the worker.

## Local validation and independent QA

| Check | Result | Boundary |
| --- | --- | --- |
| Network builder/value | 120 SDK + 65 hidden/value assertions pass | Real production Java; declared initialization/log seams. |
| Connectivity callback | 28 pass | Real manager/interface; controlled Handler/service. |
| Existing request clear | 15 pass | Reset and detached snapshots. |
| Web public/page/ownership | 28 + 21 + 29 pass | SDK caller and exact provider/destroy methods; declared transport and private ownership observations. |
| timerfd | 77 sanitizer assertions; real Linux fixture 56 pass | Production core with Windows adapter; separate Linux reference. |
| Input surface | 55 sanitizer assertions pass | Host queue seam; actual Windows QueueSurface shares 50 checks. |
| Codec receiver/waits | 16 + 36 sanitizer assertions pass | Exact production methods; declared worker/JNI seams. |
| Existing media | Timestamps 20; window ownership 31 pass | Focused regression suites. |
| Java builds | Fresh app and complete services tests javac/D8 pass | Reused verified core/framework inputs. |
| Timer cross-compilation | Five changed core/registry/ABI/test units pass warnings-as-errors | Publisher-verified LLVM-MinGW; linking is a CI gate. |
| Codec complete source | GCC/LLVM-MinGW syntax checks pass | Supplemental pinned NDK/JDK headers; final AOSP-header linking remains CI. |
| Inventory parser | 28 tests pass | Class/descriptor parser regressions. |

Independent reviewers reran tests and old-source controls. Timer QA additionally
checked blocked rearm with concurrent read/gettime, blocking aliases surviving
original-fd close and counts above 32 bits. Media QA traced QueueSurface and
AOSP Java callback paths. Web QA's discarded-ownership and page-disposal findings
were treated as blockers and repaired before completion.

Controls compile old production code with the same relevant fixture. Each of
four builder and four callback groups fails its intended contract. Initial Web
controls fail 11/21 public and 9/13 page checks; totals differ because groups
terminate early. Old timer code fails 47 assertions; InputSurface fails 8/55;
Codec receiver fails 3/16 and wait logic 7/20. The candidate adds explicit
release checks. Controls observe ordinary ownership/order, without requiring
an unsafe stale-memory crash.

The additional 29 Web ownership checks also compile against the preceding
integrated source, where 19 fail. Independent QA reproduced both results and
approved the final ownership follow-up.

ASan/UBSan were enabled. LeakSanitizer was disabled only locally because this
executor's ptrace environment prevents normal operation; CI is unchanged. The
input callback executable alone enables C++ exceptions for its deliberate
exception-control case; runtime compile settings remain unchanged.

### Windows and ART gates

Track `97-lifecycle-contracts.ps1` runs in full and restricted framework suites.
Network tests require actual native Parcel markers; callback tests also use
real Binder dispatch and HandlerThread. It runs the SDK Web caller on ART and
requires Codec's `native=true` marker, plus real `timerfd_windows_test.exe` and
`media_input_surface_callback_test.exe`, all with bounded process timeouts.
Existing guest fixtures now cover timer syscalls for both x86-64 and translated
ARM64. Exact published-head run links/outcomes are recorded in the PR after CI
execution; local tests do not establish those results.

The first Windows run of this branch exposed the fixture's missing Trace-native
bootstrap before its first dequeue assertion. The standalone entrypoint was
corrected to load `android_runtime`, matching normal app startup. Network,
WebMessagePort, timerfd and QueueSurface lifecycle gates already passed on both
Windows versions in that run. The complete suite is rerun at the corrected head;
the initial failed run is not presented as final success.

The complete Windows suite also exposed a race-prone assertion in the existing
raw-DNS app fixture: a valid result was followed by a check of the consumed
numeric descriptor. The [DNS ownership follow-up](dns-query-descriptor-lifecycle-2026-11-13.md)
replaces that observation with isolated checks of the exact production
result/cancel methods, preserves the real app/proxy paths, and fixes cleanup
after a failed poll. Its 103 production and 85 caller checks pass with
ASan/UBSan; compiled close-omission and preceding-source controls fail their
intended contracts. This test correction does not change the runtime or SDK
inventory inputs above. Final Windows results remain tied to the published
head in the PR.

## Remaining work

These are source-confirmed differences or explicitly unqualified boundaries,
not claims that a specific APK crashed.

| Priority | Source/area | Remaining contract |
| --- | --- | --- |
| High | `SettingsProvider.java` | Writes do not notify ContentService observers. Persistence, generations, projection and legacy CRUD are incomplete. The provider has null Context; notification repair needs a valid service integration. |
| High | `SyncManager.java` | Parsed parallelism does not govern cross-account admission. SyncResult retry/backoff/delay/full-sync outcomes are dropped; already-in-progress can lose scheduled work. |
| High | Provider management in `ActivityManagerService.java` | Process retention and `noReleaseNeeded=true` omit stable/unstable references; tokens/refcounts/death handling must be designed together. |
| High | `compat/media/jni` | Recorder/SoundPool still use raw JNI receiver pointers. Their Java/native release synchronization needs separate review; Codec repair does not cover them. |
| High | SDK inventory | 911 absent classes and 584 absent declarations remain in the chosen boot set. Hardware-unavailable returns alone do not repair eager class/descriptor linking. |
| High | Futex in `compat/bionic/shim.cc` | Bitset filtering is ignored; wake returns zero and chooses single/all instead of a faithful bounded wake count. WaitOnAddress needs a fuller waiter design for parity. |
| Medium | `compat/posix/src/epoll.cc` | Closing a numeric fd removes registrations even if an alias retains the open description. Timer ownership repair does not resolve this registration lifetime. |
| Medium | Sensors | Bounded event queues can evict flush markers. Sampler/flush ordering, batching latency and acquisition timestamps need repair/qualification. |
| Medium | Network builders/callbacks | Restricted/VCN normalization, explicit enterprise IDs, concurrent same-callback registrations and setup RPC under lock remain separate. |
| Medium | `LocationManagerService.java` | Geofences are accepted/logged but not monitored. Geocoder/GNSS backends and consistent unavailable callbacks remain incomplete. |
| Medium | Input modifiers | Combined bits become left-side modifiers. Right-side, Num/Scroll lock and Windows/meta state require changes in both producers and translation. |
| Medium | `compat/posix/src/ftw.cc` | CHDIR/MOUNT flags, open-fd limits and cycles do not fully follow Linux traversal contracts. |
| Medium | ELF/bionic | Library unloading and missing imports remain compatibility boundaries; successful loading does not prove every native call resolves/works. |
| Medium | Clocks/process/POSIX | CRT local-time range/transitions, suspend clocks, timer cancellation, fork/namespaces/signals/FIFOs/xattrs remain partial or unqualified. |
| Medium | SurfaceTexture/graphics | External textures, fences, dataspace/transforms and multi-buffer behavior need more than callback/window lifetime fixes. |
| Qualification | Hardware/architecture/APKs | Physical camera/audio/sensors, native Windows ARM64, arbitrary APK/native-library corpora and real Android differential tests are not exhaustively qualified. Translated ARM64 is a different path. |
| Qualification | WebView2 | Full Android Chromium and JS transfer/rejection/lifecycle behavior remain outside the Java fixtures. |

Useful next coherent work units are settings observers, sync admission/results,
remaining media receiver lifetimes, sensor flush ordering and epoll description
identity. Each needs old/new controls and an actual Windows gate; adding methods
that merely return defaults does not establish the contracts.

## Reproduction and primary sources

Run `tools/java_api_inventory.py`, `tools/java_member_inventory.py` and
`tools/audit_services_jni.py` using the JSON's ordered inputs and hashes.
Replace only `gesher-app.jar` for the candidate comparison. Behavioral runners
under `tools` are `test_network_builder_contracts.py`,
`test_connectivity_callback_contracts.py`, `test_webmessage_ports.py`,
`test_timerfd_contracts.py`, `test_input_surface_callbacks.py` and
`test_codec_lifecycle_contracts.py`; their `--help` describes required verified
framework/core/SDK/JDK inputs and controls. Native runners accept `--sanitize`;
old-source controls use baseline `6bfc980c335a8b8ae1a9271dc5da43078561b516`.

- [Android NetworkCapabilities](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/NetworkCapabilities.java), [NetworkRequest](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/NetworkRequest.java), [ConnectivityManager](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/ConnectivityManager.java).
- [Android WebMessagePort](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/webkit/WebMessagePort.java).
- [HTML message-port posting algorithm](https://html.spec.whatwg.org/multipage/web-messaging.html#message-port-post-message), read on 2026-11-13; recipient-self and no-recipient sends consume serialization/transfers before returning.
- [Immutable historical Chromium AppWebMessagePort](https://chromium.googlesource.com/chromium/src/+/2d145fe56a1d995fc1a016da03c1f91f217612e0/content/public/android/java/src/org/chromium/content/browser/AppWebMessagePort.java), a state/ownership reference, not the claimed current Android WebView version.
- [Android MediaCodec.java](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/java/android/media/MediaCodec.java), [MediaCodec JNI](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/jni/android_media_MediaCodec.cpp).
- [Linux v6.6 timerfd](https://github.com/torvalds/linux/blob/v6.6/fs/timerfd.c), [ktime](https://github.com/torvalds/linux/blob/v6.6/include/linux/ktime.h).
- [SetThreadpoolTimer](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-setthreadpooltimer), [WaitForThreadpoolTimerCallbacks](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-waitforthreadpooltimercallbacks).
- [DEX format](https://source.android.com/docs/core/runtime/dex-format), [JVM resolution](https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-5.html#jvms-5.4.3).
