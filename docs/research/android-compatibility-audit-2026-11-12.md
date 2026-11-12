# Android and Windows compatibility audit, 2026-11-12

This pass starts from `2bc86c0ae067b43a74bf76c5010a52b4564e81e5` on
`main`. It combines three independent source-mapping investigations, exact
SDK/boot-DEX inventories, focused repairs, old-source negative controls and
independent review. The Android reference is the project's pinned
`android-16.0.0_r4`; the public signature reference is Android SDK 36.
The [November 8 audit](android-crash-contracts-2026-11-08.md) remains the
record of earlier repairs and their CI evidence.

**Compatibility remains partial.** This audit does not certify every line,
every overload, every hardware configuration or every possible APK. In
particular, class presence, a resolved native symbol and a nonthrowing Default
service answer establish different things; none establishes end-to-end behavior.
Android's own [CTS description](https://source.android.com/docs/compatibility/cts)
distinguishes API signatures from behavioral compatibility.

## Repository and execution map

| Layer | Principal source | Responsibility and boundary to inspect |
| --- | --- | --- |
| Pinned platform | `aosp.lock`, `patches/`, `tools/framework/`, `cmake/` | Builds AOSP ART, libcore, framework and graphics against Windows adaptations. Generated classes and patches are part of the runtime contract. |
| APK installation and launch | `app/java/com/gesher/app/`, `app/java/com/gesher/server/ApkPackage.java`, `Installer.java`, `PackageInstallerService.java`, `Zygote.java` | APK/split parsing, signatures, components, code generations, resources, native extraction, process creation and the guest/Windows path namespace. |
| Binder and lifecycle | `compat/binder/`, `compat/posix/src/binder*`, `ActivityManagerService`, `ActivityTaskManagerService`, `ActiveServices`, `BroadcastController`, `PendingIntents` | Named-pipe transport, object/descriptor ownership, caller identity, task/process lifetime, dispatch, results, cancellation and completion. |
| Java services | `app/java/com/gesher/server/`, `Services.java`, `SystemServer.java`, `StandIn.java`, `app/gen/` | Generated AIDL delegates plus handwritten state/backends. Default answers need separate contract review, especially asynchronous methods. |
| Module API facades | `app/java/android/`, `app/compat-java/` | Connectivity, Wi-Fi, Bluetooth and other absent module entry points; signatures, values, exceptions and capability reporting require independent checks. |
| Guest native ABI | `compat/bionic/`, `compat/elf/` | Android LP64 layouts, System V calling conventions, native exports, ELF loading/relocations/TLS and constructors/finalizers. |
| Host OS adaptation | `compat/posix/`, `compat/libcore/`, `compat/net/` | CRT/Win32 descriptors, Winsock, waits, mappings, process pipes, DNS, trust stores and filesystem semantics. Host and guest adaptations are distinct surfaces. |
| ARM64 | `compat/arm64/` | ARM64 ELF on x64 uses Dynarmic and host thunks. Native Windows ARM64 uses another bridge with different TLS/variadic limitations. |
| Graphics and input | `compat/hwui/`, `compat/opengl/`, `compat/angle/`, `compat/input/`, Java window/input bridges | Skia/HWUI, GLES through ANGLE/D3D, image storage and parcels, surfaces, Win32 input, TSF/IME, focus and callbacks. |
| Media and devices | `compat/media/`, `compat/camera/`, `compat/devices/` | AOSP/software extractors, Media Foundation, WASAPI, camera queues, sensors/location, data ownership and hardware capability/lifetime. |
| WebView and desktop | `compat/webview/`, `compat/windows/`, `compat/shell/`, `compat/accessibility/`, `compat/credentials/` | WebView2 COM/DevTools, file selection, clipboard, drag/drop, shell integration, notifications, UI Automation and credential storage. |
| Verification and delivery | `tests/`, `tools/`, `.github/workflows/`, installer sources | Host contracts, real Windows ART and ELF tests, translated ARM64 execution, provenance-checked build artifacts and packaging. |

The baseline has 86 server Java files, 52 app-bridge Java files, 75 handwritten
Android facade files and 27 compatibility placeholder files. These are source
counts, not counts of implemented APIs. The source-mapping investigations read
the existing architecture/status reports and traced selected high-risk paths
through their callers and backends; they did not independently review every
line with equal depth.

The `app/java/com/google/` classes are client test stubs explicitly excluded
from the production boot JAR by `app/build.sh`. Findings in those classes must
not be presented as shipped GMS runtime defects.

## Exact baseline API inventory

The machine-readable [inventory](android-compatibility-inventory-2026-11-12.json)
contains input hashes, every absent boot class, every absent public/protected
member on a present class, and the possible flag mismatches. It uses the actual
five boot DEX JARs from successful baseline CI run
[42677450306](https://github.com/gesher-runtime/gesher/actions/runs/42677450306),
not a source-file name scan or an unrelated local build.

| Static measurement | Count | Interpretation |
| --- | ---: | --- |
| SDK 36 class definitions | 6,227 | Public SDK input, including annotation types. |
| Available on the selected boot path | 5,303 | Definition found; methods and behavior still require review. |
| Absent from that boot path | 924 | An optional module, uses-library or APK provider may supply a class elsewhere; those providers were not evaluated by this inventory. |
| Public/protected member declarations | 90,630 | Constructors, fields and methods, compared using exact JVM descriptors and inheritance lookup. |
| Exact descriptor present without a flagged difference | 82,055 | Static presence only. |
| Absent member on a present class | 557 | 287 methods, 268 fields and 2 constructors; this is a concrete linkage-review backlog. |
| Member whose declaring class is absent | 7,516 | Overlaps the absent-class inventory; do not add this to the class count as separate defects. |
| Descriptor present with a possible flag mismatch | 502 | Predominantly new final restrictions; needs source/build and usage review, not automatically a runtime failure. |

Absent classes by family: connectivity modules **266**, other Android framework
families **212**, Health Connect **207**, AdServices **154**, Bluetooth **41**,
NFC **33**, SDK sandbox **9**, and AndroidX annotations **2**. The selected
`java.*`, `javax.*` and `dalvik.*` public SDK class definitions were all found.
That last result does not establish method, JNI or behavioral completeness.

The 557 absent members are concentrated in these already-present facades:

| Class | Absent declarations | Class | Absent declarations |
| --- | ---: | --- | ---: |
| `WifiManager` | 171 | `BluetoothAdapter` | 69 |
| `BluetoothDevice` | 56 | `ScanResult` | 44 |
| `WifiInfo` | 38 | `WifiConfiguration` | 35 |
| `SdkSandboxManager` | 26 | `TetheringManager` | 25 |
| `HealthConnectManager` | 24 | `NetworkStats.Bucket` | 21 |
| `MediaStore` | 17 | `BluetoothHeadset` | 13 |
| `NetworkStatsManager` | 8 | `BluetoothManager` | 4 |
| `ConnectivityManager` | 4 | `ConnectivityManager.NetworkCallback` | 1 |
| `WifiManager.WifiLock` | 1 | | |

These numbers are deliberately not converted into a compatibility percentage.
Some absent APIs describe unavailable optional hardware; others are ordinary
software entry points that an app may resolve during startup. Supplying every
signature with a null/no-op body would not satisfy the latter contracts.

### Additional public-API review during CI

A second pass traced 23 absent declarations in MediaStore, ConnectivityManager
and WifiLock against the pinned module sources. It adds **11 exact SDK 36
declarations** in this PR; the inventory above remains the reproducible baseline,
not a new measurement of the repaired boot DEX:

| Facade | Added declarations | Contract and boundary |
| --- | --- | --- |
| `MediaStore` | `createWriteRequest`, `createDeleteRequest`, `createTrashRequest`, `createFavoriteRequest` | Preserve URI order, construct the pinned ClipData/ContentValues envelope, and return the provider's PendingIntent. The provider owns approval and execution. |
| `MediaStore` | `markIsFavoriteStatus`, `isCurrentSystemGallery`, `isCurrentCloudMediaProviderAuthority`, `isSupportedCloudMediaProviderAuthority`, `notifyCloudMediaChangedEvent` | Forward the exact provider method, arguments and Bundle types; preserve permission denial, missing-provider and malformed-result behavior. |
| `ConnectivityManager` | `setNetworkPreference(int)` | The deprecated pinned Android method also performs no action. This does not change routing. |
| `ConnectivityManager.NetworkCallback` | `onReserved(NetworkCapabilities)` | Restore the empty overridable base callback. This does not implement `reserveNetwork`. |

The call-site fixture is compiled separately against the public SDK, then run
against production classes and a controlled provider through the real
ContentResolver. The old implementation fails with `NoSuchMethodError`; the
replacement passes 208 assertions. Its resolver context targets API 36; the
standalone host fixture does not set ART's process-wide target SDK or establish
a working system MediaProvider installation.

The other 12 reviewed declarations need more backend work: eight MediaStore
methods for AppOps-based management, document/media conversion, redaction and
cloud descriptor opening; ConnectivityManager keepalive, reservation and
watchlist methods; and held WifiLock WorkSource attribution. Returning an
unredacted original URI or manufacturing an approval token would not implement
these contracts. No references to the 23 declarations were found in the
examined baseline framework/app compile JARs; the demonstrated linkage failures
are SDK-compiled app calls, not proven internal framework call sites.

### Service and JNI inspection queue

The baseline source/framework scan found 69 service implementation classes,
66 corresponding interfaces in the supplied compile input and 1,048 explicit
methods. Its classifications are 864 bodies awaiting semantic verification,
12 explicit empty collections, 80 default answers, 78 no-ops and 14 explicitly
unsupported paths. It also found 2,768 inherited Default entries by method-name
heuristic, 50 direct service registrations and 198 Context-key candidates.

The JNI scan found 174 upstream registration rows: 50 direct local symbol
references, 4 known alternate registrations and 120 without a direct local
reference. It recorded 523 source stand-in entries. These are **review queues**:
compiled AOSP sources, aliases, registration ordering and replacement of early
stand-ins prevent interpreting these totals as confirmed missing active natives.
The service scanner's historical `date` label remains November 5; this run is
identified by `generated_at_utc` on November 12 and the baseline Git SHA.

## Repairs in this change

| Area | Confirmed failure | Repair and maintained evidence |
| --- | --- | --- |
| PendingIntent completion | Both service and direct sender paths discarded completion receivers/result codes. Callback-dependent callers could wait indefinitely. | Carry callbacks through dispatch. Broadcast completion uses the existing acknowledged delivery engine and final results; nonbroadcast completion follows dispatch. Tests cover public `OnFinished`, direct sender, empty/async broadcasts, timeout/death and one-shot/generation interactions. |
| PendingIntent cancellation | Listener registration reported success without retaining a listener. | Store and retire actual per-record callbacks, dispatch cancellation outside record locks, isolate callback failures, and retain generation/authorization checks. |
| PendingIntent identity and mutation | Resolved MIME type and retained creation flags were missing from identity; fill-in rules were ignored; UPDATE_CURRENT replaced the original final Intent's non-key flags. | Strip only lookup/control flags, preserve MIME identity and fill-in decisions, and update extras/preceding stack entries while retaining the final Intent's original flags. This does not implement multi-activity launch or activity-result senders. |
| Clipboard listeners | Nested or concurrent notification invoked `RemoteCallbackList.beginBroadcast()` while an earlier broadcast remained active. | Snapshot under synchronization, finish the broadcast, then deliver callbacks outside the lock. Actual framework tests reproduce the old reentrant failure. |
| Content observer lifecycle | Death could arrive before a registration was published, leaving a dead observer retained. Independent QA also reproduced stale death/RemoteException cleanup removing a replacement registration and Binder equality callbacks running inside the registry lock. | Track retirement before publication and on removal; bind cleanup to the exact registration, use canonical Binder identity, preserve first-seen delivery order, and unlink/deliver outside the registry lock. Existing owner and URI authorization checks remain. Local callback exceptions no longer suppress unrelated listeners. |
| pthread TLS keys | Deleting/reusing a key exposed another generation's thread value and could call a new destructor on a stale object. | Track generations in global slots and thread values; validate under the key lock, invoke destructors outside it, retain four-round cleanup behavior and retire exhausted generations without wraparound. |
| poll/select time budgets | Duration multiplication overflowed, invalid times were accepted, and select discarded the remaining budget. | Validate split seconds/nanoseconds, use monotonic elapsed budgets and repeated finite wait slices, preserve descriptor interests and the remaining select timeval. Signal-mask atomicity remains outside this repair. |
| getaddrinfo allocation failure | Failure while converting a later address returned a successful partial list. | Transactional conversion: release the prefix and backend list, return Android's memory error and leave the output null. Fault-injection tests cover each allocation and all wrapper routes. |
| clock_nanosleep | Negative and huge durations reached an incorrect Windows wait or signed overflow, long waits returned after one bounded slice, and unknown clock IDs silently succeeded. | Validate clocks and intervals, retain deadlines in 128-bit arithmetic, repeat finite waits, recheck absolute wall-clock deadlines, preserve errno and report interrupted relative remainders through the existing signal adapter. Unsupported clock backends return an error. |
| CPU affinity masks | Host `sched_getaffinity` wrote whole words beyond short caller allocations; the guest version silently accepted truncated masks. | Validate nonzero whole-word capacity and null pointers before writing; fill only within the declared size. This does not add per-thread affinity or processor-group topology support. |
| sendfile progress | With a null offset, the host consumed a whole input block before knowing whether the output accepted it. A blocked socket returned EAGAIN after consuming 65,536 input bytes; a partial write returned 8,064 while consuming 65,536. | Use positioned input reads and commit the input cursor or explicit offset only for bytes written. Stop zero-progress writes; retain earlier progress on later errors. The compound operation does not serialize concurrent users of a shared descriptor. |
| freopen mode translation | Host mode parsing discarded exclusive creation before `_wfreopen`, turning `wx` into destructive `wb`. The guest's `e` to `N` translation was then discarded by the host Linux-mode parser. | Pass the original guest mode into the host mapper and preserve exclusive `w`/`w+` creation and no-inherit flags in the final CRT mode. Let the CRT perform the exclusive open atomically; retain existing stream/descriptor retirement transactions. |
| ELF finalizers | The host close route called ARM64 guest finalizer addresses as host code; recursive close could execute callbacks twice. | Dispatch through the translator for ARM64, claim close once while preserving global symbol visibility until callbacks finish, run the reverse finalizer array before DT_FINI and ignore null/-1 sentinels. Existing native/translated fixtures now check actual destructor order and repeat close. |
| Native APK library filenames | ZIP's forward-slash test did not account for Windows backslashes, device names and forbidden filename syntax. | Validate all selected direct library entries before writing any output. Ordinary Unicode names remain supported; invalid Windows names are rejected without overwriting earlier destination files. This is not full Android filename virtualization. |
| AAudio arguments/timeouts | Null/negative transfers reached backend buffer arithmetic; large deadlines overflowed. | Return pinned public error codes, handle zero transfers without backend access and saturate positive deadlines safely. Existing playback/capture NDK fixtures exercise exported argument contracts. |
| MediaExtractor source ownership | Repeated setup replaced source/extractor ownership while selected readers retained the first source; failed setup could retain incomplete state. | Reject a second setup before opening/replacing its source; publish ownership only after first setup succeeds. Actual WAV tests check rejection, retained sample access and retry after initial failure. |
| YUV crops and JPEG rows | Bounds overcounted the unused end of the last row, rejecting valid bottom/right crops. Independent QA then reproduced a real libjpeg out-of-bounds read for a valid 30-pixel crop because component rows lacked DCT-block padding. | Validate each plane's actual last-row extent and pad temporary JPEG component rows by repeating their final sample. Public tests encode/decode ordinary, narrow and padded-stride crops and check dimensions/pixels. |
| Bitmap parcels | A serialized size could claim more pixel storage than the actual mapped SharedMemory buffer; several failures skipped deterministic cleanup. | Check the actual direct-buffer capacity before copying. Unmap/close through one owner while retaining the first Java exception. Real shared/inline pixel roundtrips and undersized-mapping rejection run in Windows ART. |
| WebView upload paths | `/mnt/<drive>` and `/mnt/unc` became incorrect local Windows paths. | Reuse `Zygote.fromWindowsPath/windowsPath`, including native Windows input, Unicode, drive and UNC rules. Checked-JNI tests run the actual production JNI function; Windows runs the registered native method. |

Primary Android references include pinned
[PendingIntentRecord](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/PendingIntentRecord.java),
[PendingIntentController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/PendingIntentController.java),
[pthread keys](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/pthread_key.cpp),
[poll/select](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/poll.cpp),
[linker finalization](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/linker/linker_soinfo.cpp),
[AAudio](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-16.0.0_r4/media/libaaudio/src/core/AAudioAudio.cpp)
and [NuMediaExtractor](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-16.0.0_r4/media/libstagefright/NuMediaExtractor.cpp).
The second pass also uses pinned
[MediaStore](https://android.googlesource.com/platform/packages/providers/MediaProvider/+/refs/tags/android-16.0.0_r4/apex/framework/java/android/provider/MediaStore.java),
[ConnectivityManager](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/ConnectivityManager.java),
[ContentService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/content/ContentService.java),
[clock_nanosleep](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/clock_nanosleep.cpp)
and [sched_getaffinity](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/sched_getaffinity.cpp).
File/stream references include pinned
[Android stdio](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/stdio/stdio.cpp),
[mode parsing](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/upstream-openbsd/lib/libc/stdio/flags.c)
and the Linux man-pages project's
[sendfile contract](https://kernel.googlesource.com/pub/scm/docs/man-pages/man-pages/+/52b7bab3a44fb5d2d464fa8acd6beee3b62269aa/man/man2/sendfile.2).
The content observer tests establish specific Gesher regressions; modern AOSP's
RemoteException cleanup policy and native Binder scheduling are not identical
to this implementation.
Windows filename validation follows Microsoft's
[file naming rules](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file).
The stdio mapping uses the CRT's documented
[`freopen` exclusive-create mode](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/freopen-wfreopen)
and [`N` no-inherit mode](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen).

## Validation and reproduction

The full changed app and service-test sources compile and pass D8 against the baseline's actual
framework/core class inputs. The local source checkout used for optional-module
generation is a pinned subset; the resulting local JAR is a compile check,
not a release runtime or a replacement for the full CI source build.

| Maintained check | Local evidence / role |
| --- | --- |
| `tools/test_pending_intent_callbacks.py` | 82 PendingIntent, 11 clipboard and 31 content observer assertions against actual production/framework classes, including permission/creator fences and deterministic death/replacement races. The declared host adapter replaces only native boundaries; it is not a Windows Binder/clipboard run. The content case isolates the real observer registry from SyncManager/process construction. |
| `tools/test_media_store_requests.py` | 208 assertions, separately SDK-compiled call sites, exact provider envelopes, return values, exceptions and release behavior. A controlled IContentProvider is the declared boundary; no substitute approval token is created by production code. |
| `tools/test_bionic_key_contracts.py` | Production key/destructor functions, cross-thread reuse, callback mutation, cleanup order, host TLS exit model and generation exhaustion. |
| `tools/test_bionic_wait_contracts.py` | Production public aliases, invalid times, actual ready descriptors, controlled huge durations and multiple finite wait slices. |
| `tools/test_bionic_dns_contracts.py` | Production ABI conversion and allocation-failure cleanup, with backend/prefix ownership checks. |
| `tools/test_ordinary_os_contracts.py` | 179 assertions over production clock and host/guest affinity functions under controlled Windows boundaries and ASan/UBSan, plus 40 in an independent Linux C reference. Covers complete/adjusted deadlines, interruption and extreme durations, 1–1,025 processors and short/dynamic mask capacities. The public C fixture is included in both guest libc paths for Windows execution. |
| `tools/test_sendfile_contracts.py` | 167 ASan/UBSan assertions over the extracted production adapter, real local files and nonblocking socket payloads, and controlled partial/error/zero-write boundaries; 33 in an independent Linux reference. Shared public contracts cover cursor/offset and exact payload preservation through the guest entry point. |
| `tools/test_freopen_mode_contracts.py` | 329 assertions for each extracted host and guest route, including CRT and descriptor-transaction boundary observations, plus 160 in an independent Linux public fixture. Shared guest contracts check existing-file preservation, exclusive creation, binary bytes, stream retirement and close-on-exec modes. The host CRT model does not replace Windows execution. |
| `tools/test_elf_finalizers.py` | Eleven production-close/global-lookup assertions, including reentry, concurrent claims and an observing translator boundary. Actual ARM64 execution belongs to the existing Windows translated fixture. |
| `tools/test_media_api_contracts.py` | 266 production-source assertions: AAudio 37, extractor ownership 17, YUV admission 32 and raw JPEG row/padding values 180, under ASan/UBSan. The observer does not substitute for real codecs. |
| `tools/test_native_library_paths.py` | 96 assertions using real ZIPs, ordinary/Unicode libraries, ABI selection, invalid names (including console devices and padded device stems) and existing-file preservation. |
| `tools/test_webview_paths.py` | 26 checked-JNI assertions over the production native entry point and real Java path mapper. Executable location is adapted to a fixed D: host envelope. |
| `tests/native/libc_test.c` and trivial ELF fixtures | Shared public contracts wired into the existing x86-64 and ARM64 guest builds and Windows ABI/translator runners. Final-head compilation and execution must be confirmed by CI; these are not local Windows results. |
| `tests/windows/tracks/94-framework-fixes.ps1` | Public PendingIntent, clipboard, content observer and MediaStore paths, actual WAV/JPEG processing, Bitmap/SharedMemory transport, native extraction and registered WebView path conversion in fresh ART VMs. The existing `ContentSecurityTest` remains a separate required gate. |

Independent media QA additionally built the pinned Android 16 libjpeg-turbo
portable implementation with ASan and executed the actual encoder. Both NV21
and YUY2 30-pixel crops reproduced out-of-bounds reads before row padding;
widths 2, 14, 24, 30 and 32 pass after it. The maintained host fixture checks
component samples and full-DCT padding, and Windows exercises the real codec.
The Windows PendingIntent fixture uses generated AIDL and local Binder/Parcel;
it does not by itself prove remote named-pipe callback IPC.

Old-source controls against the baseline fail on the selected Java, native,
media, extraction and WebView contracts. These show that tests distinguish
the repaired behavior; they do not establish a third-party APK crash count.
Host sanitizer runs disable **only LeakSanitizer locally**, because process
inspection is unavailable under this executor. CI retains normal sanitizer
configuration. No leak-check result is inferred from local ASan/UBSan success.

The existing allocation, directory, semaphore, file-lock, socket-control,
positioned-I/O, memfd, media-retriever and UTC host regressions were rerun before
repair; all ten suites passed. The existing checked-JNI audio-buffer suite
also passed. Mandatory Windows build and runtime gates remain enabled;
their status must be read from the PR's exact head run, not inferred from the
green baseline run or from host fixture results.

The first PR CI attempt exposed NDK clang's `nonnull` diagnostic in deliberate
invalid-buffer tests. The tests now obtain the null buffer at runtime through a
volatile pointer, retaining the invalid-input checks and the compiler's normal
warning policy. Production buffer validation and CI acceptance gates are not
relaxed.
Independent native QA also caught the GNU feature macro required by the NDK's
affinity declarations; the shared libc fixture now enables those declarations
before including any system header. Host-only compilation had hidden that
guest-build difference.
File QA also corrected the order of `sendfile` range validation: the original
count and offset range must be checked before applying the per-call transfer
cap. Exact old-source controls expose the previous cursor loss, zero-write
loop and signed-offset overflow. The three new public C fixtures also compile
and run together against Linux, passing 233 checks without helper collisions
or cross-fixture state failures.

To reproduce the static inventory, run the following commands **from a checkout
of baseline `2bc86c0ae067b43a74bf76c5010a52b4564e81e5`**, with the collected
baseline files at the indicated paths. The service/JNI scanner reads source
from its own checkout; invoking it on this repaired branch produces a new
source inventory instead of reproducing the published baseline.

```sh
python3 tools/java_api_inventory.py --sdk-jar android-36/android.jar \
  --sdk-api-level 36 --boot-jar core-oj.jar --boot-jar core-libart.jar \
  --boot-jar core-icu4j.jar --boot-jar framework.jar --boot-jar gesher-app.jar \
  --outdir audit/classes
python3 tools/java_member_inventory.py --sdk-jar android-36/android.jar \
  --sdk-api-level 36 --boot-jar core-oj.jar --boot-jar core-libart.jar \
  --boot-jar core-icu4j.jar --boot-jar framework.jar --boot-jar gesher-app.jar \
  --outdir audit/members
python3 tools/audit_services_jni.py --framework-jar framework-classes.zip \
  --aosp-dir third_party/aosp --out audit/services-jni.json
```

SDK `android.jar` SHA-256:
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`.
The framework compile ZIP SHA-256 is
`40791ee9b97cefbd5c7358860af2d601a4ce89f2f6e1653e20162aeae953253a`.
Individual boot-JAR hashes and generator hashes are in the inventory.

## Remaining work, ordered by likely failure mechanism

| Priority / family | Remaining gap | Evidence needed for a complete implementation |
| --- | --- | --- |
| Startup/linkage | The baseline's exact 924 absent boot classes and 557 members above; this PR supplies 11 of those declarations. Module/provider availability must be resolved per feature. | A fresh inventory of the final built boot path, actual provider/class-loader placement, exact descriptors, values/exceptions and callback tests. |
| Native architecture | 32-bit ARM/x86 APK native libraries remain unsupported; native Windows ARM64 TLS/variadic bridging differs from the translated x64 path and has substantial unfinished implementation. | Separate ABI fixtures, per-thread TLS allocation/publication, variadic calls and real device/runner execution. |
| Native loader | ELF ranges/dynamic structures need comprehensive bounded validation; namespace/version/FD-offset loading and reference-counted unloading remain incomplete. | A malformed/truncated corpus, dependency/namespace fixtures and real unload/reload ownership tests. This patch changes finalizer dispatch only. |
| Activity semantics | PendingIntent multi-activity stacks, activity-result senders and feature/token/result identity remain incomplete; canceled-token metadata still uses active-record lookup. | Real ActivityThread/task-stack/result delivery with separate capability and metadata lifetime. |
| Callback/state services | Inherited Default methods can omit asynchronous completion. Settings state/notifications and observer integration with real processes and sync backends need broader work. | Per-method Android contract review and race-aware public manager/Binder fixtures. |
| OS behavior | Atomic ppoll/pselect signal masks, asynchronous signals, process-shared synchronization, cancellation/scheduling, real affinity restrictions, unsupported sleep clocks and some host filesystem semantics remain incomplete. Existing MONOTONIC and BOOTTIME both use QPC, whose suspend-time semantics differ from Linux MONOTONIC. `sendfile` does not serialize shared-cursor users; `freopen(NULL, ...)` mode changes and Android's append-exclusive `ax/a+x` modes remain outside the bounded stdio repair. | Full host/guest call-path tests, suspend/clock changes, cancellation and Windows handle-lifetime checks. The new sleep/mask checks do not establish complete Linux scheduler emulation. |
| Epoll and Unix transport | Edge-trigger/nested/descriptor-alias semantics and SCM_RIGHTS remain limited. Unsupported ancillary sends are rejected; that does not implement descriptor transport. Blocking record locks remain explicitly unsupported. | Real socket/descriptor lifecycle and blocking/interruption tests on Windows. |
| Native platform families | NDK media surface, Vulkan, DRM, camera2 NDK and other native modules have separate missing exports/backends. | Current NDK symbol inventory plus actual execution; historical missing-symbol totals are not reused as fresh measurements here. |
| Media lifecycle/hardware | AAudio active-callback policy, disconnection/error callbacks, route/builder policies, concurrent extractor release and unusual device/codecs need further work. | Real ring/payload, unplug, concurrent release and camera/audio device tests. |
| Shared graphics storage | SharedMemory protection masks are not fully enforced; ImageReader, SurfaceTexture, fences and cross-owner/context lifetime need wider coverage. | Real protection, queue/retirement and context/window destruction tests. |
| WebView | Subframes/origin identity, message ports, permissions, cookies and teardown require broader integration testing. | Actual multi-frame browser tests, deferral/callback cancellation and file-upload byte verification. Path-string correctness alone does not prove upload success. |

The existing open microG stability work is separate from this audit. These
changes retain the existing identity, permission, source-provenance and CI
acceptance gates; they do not turn unavailable features into successful no-ops
to improve an inventory or test score.
