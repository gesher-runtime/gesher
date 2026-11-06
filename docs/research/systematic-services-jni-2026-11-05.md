# Systematic Binder/service and JNI surface audit, 2026-11-05

Gesher has substantial implemented service paths, alongside broad inherited
defaults. Successful startup, an exported native symbol, or a registered
Binder name does not establish the Android method's full contract. This audit
examines that distinction independently of which apps happened to crash.

The machine-readable inventory is
`out/compat-audit/systematic-services-jni-2026-11-05.json`. It includes per-method
rows, exact source paths/lines, normalized source-text hashes, source commit,
generation time, registration candidates, and reviewed gap families. Regenerate:

```powershell
python tools/audit_services_jni.py `
  --framework-jar out/microg-build-inputs/framework-classes.zip `
  --aosp-dir out/native-build/aosp `
  --out out/compat-audit/systematic-services-jni-2026-11-05.json
```

The native registration reference is pinned AOSP `android-16.0.0_r4`,
`frameworks/base/core/jni/AndroidRuntime.cpp`. Binder APIs are read with javap
from an existing framework Java-class compile input, SHA-256
`f325c624daa7cd4c684773be6fed61053138d9e5f5ee8701df154d761518c3df`.
That input has no independently established release/source receipt here. It
is separate from the installed DEX framework.jar and from the newly built
native runtime. The source inventory includes current user work in progress;
its exact hashes matter when comparing an installed build.

The snapshot covers over 60 Default/Stub subclasses or direct Default instances
under app/java, approximately 770 explicit methods, and approximately 3,000
interface methods inherited from generated Default implementations. Exact
counts are in the JSON because concurrent source work changes them. It also
records approximately 45 direct central service registrations, 174 upstream JNI registration
rows, and 716 device/attach JNI stand-in rows.

## Highest-value families to address

| Priority | Family | Concrete gap or limitation | Evidence |
| --- | --- | --- | --- |
| 1 | Default Binder dispatch and callbacks | Missing overrides answer null/0/false, or an empty list, and void methods can do nothing. An asynchronous request or observer registration can therefore produce no callback; an object-returning method can violate a framework manager's assumptions. Strict mode exposes such calls but is not a production implementation. | `app/java/com/gesher/server/StandIn.java:145`; individual interface methods in JSON |
| 1 | Lifecycle, packages, windows, notifications, audio | These classes implement important paths while inheriting many additional methods. Review observers, channel/group APIs, task transitions, route/policy queries and callback/error completion as contracts, rather than equating constructor success with complete support. | `ActivityManagerService.java:49`, `PackageManagerService.java:32`, `WindowManagerService.java:22`, `NotificationManagerService.java`, `AudioService.java:37` |
| 1 | Service construction and source/installed skew | Current source registers deviceidle using IDeviceIdleController.Default. The installed PhoneDoctor failure was a DeviceIdleManager-null constructor path, as verified separately by the parent investigation. A source fix is not installed merely because it exists in this tree; the entire Default controller still needs an explicit desktop policy. | `app/java/com/gesher/server/Services.java:129`; parent crash evidence |
| 1 | Settings and system activity intents | Two notification Settings actions have a Windows fallback. Generic Android Settings pages, battery-exemption, app-details/all-files permission flows, and camera-capture actions have no general built-in fallback here; they require an installed matching activity. Unresolved starts return START_INTENT_NOT_RESOLVED. | `app/java/com/gesher/server/ActivityTaskManagerService.java:589` through its unresolved-start branch; exact anchors in JSON |
| 1 | Documents/SAF contract | OPEN_DOCUMENT/GET_CONTENT/OPEN_DOCUMENT_TREE use Windows dialogs and return file:// with grant flags. This is an implemented picker, but does not establish DocumentsProvider document IDs, persisted tree permissions, MIME filtering or multiple-selection semantics. | `app/java/com/gesher/server/ActivityTaskManagerService.java:620`; Uri.fromFile anchor in JSON |
| 2 | Policy, shortcuts, usage, diagnostics | DevicePolicyService, ShortcutService, UsageStatsService and DropBoxManagerService are empty Default subclasses. Comments describing device state do not implement further methods. These include software contracts independent of physical hardware. | `app/java/com/gesher/server/DeviceServices.java:184`, `:191`, `:198`, `:272` |
| 2 | Input/accessibility/camera lifecycle | App-local services and real Windows bridges exist. They still require per-method validation, including callback/death/reconnect behavior. Accessibility and camera must not be marked absent simply because their names are missing from the central services map. | `app/java/com/gesher/app/input/VirtualKeyboard.java:50`; `app/java/com/gesher/app/accessibility/AccessibilityService.java:71`; `CameraService.java:103` |
| 2 | Native registration holes | LocalSocketImpl, MemoryIntArray, MemoryFile and Scrypt upstream registrations have no matching local registration symbol or selected source mention in the reviewed compat/cmake/app-java search. These are candidates for targeted class/native contract probes, not proof that every referenced class is present or every API needs implementation. | Upstream AndroidRuntime.cpp lines and negative-search scopes in JSON |
| 3 | Unsupported hardware policy | Cellular/SIM/subscription services inherit Default, and the vibrator service intentionally reports no vibrator. Such hardware absence should be explicit and consistent. A missing completion callback remains a different question from a missing cellular radio. | `app/java/com/gesher/server/DeviceServices.java:205`, `:212`, `:237`, `:243`; `Services.java:154` |

## Native registration findings need registration-order review

Of 174 upstream AndroidRuntime registration rows, 124 registration symbol names
are not referenced by local compatibility sources. That is a candidate list,
not 124 proven missing features. Gesher deliberately uses alternate entry points
for graphics, media, shared memory, NativeActivity, camera/sensors and service
natives. The raw rows preserve this limitation rather than assigning a false
coverage percentage.

The 716 stand-in method rows split into 523 committed attach fallbacks and 193
generated device entries. Of those, 655 target zero/noop return helpers and 61
use special stand-in functions. Attach fallbacks register before real graphics
and input registrations; later registrations replace many of them. Therefore
these counts are an inventory of fallback code, not a count of active failures.
The generated source and its method targets are included so that each family
can be reconciled with registration order and compile-time feature selection.

JNI_OnLoad_android_runtime checks a prerequisite class before each registration.
If the class is absent it clears the exception and skips that registration.
Loading android_runtime successfully thus does not prove every native method
has a registered implementation.

## Other explicit desktop defaults

ACTION_CHOOSER unwraps the target instead of presenting an Android share sheet.
SettingsProvider stores values in memory until the server exits, without
generation tracking. AppOps methods return compatibility policy answers,
including MODE_ALLOWED in the implemented check/note/start paths; this is not
Android's full operation history, observer, routing or policy service.
These are documented differences to evaluate against needed contracts.

## Limits and next verification step

This is source/API analysis, not runtime semantic conformance. Explicit method
bodies are marked implemented_unverified or classified as simple defaults/noops.
Inherited missing-override comparison uses method names, not full overload/type
resolution; callback/helper classes and direct Default instances are listed
separately. Some APIs can be intentionally unsupported or handled in another
layer. AIDL interfaces absent from the supplied class input stay unavailable
instead of being counted as implemented.

The roughly 200 Context service-key candidates are deliberately unranked. Some managers
are local, use other Binder names, or are injected into app-local caches. They
must not be reported as that many absent services. This audit covers direct central
registrations, not every Android service discovery mechanism.

The tool records generated/attach stand-in input availability separately, so
an unavailable generated file is not treated as an empty inspected table. It
also hashes the generator. A separate source-only scan of the isolated
candidate is saved as `out/compat-audit/candidate-service-source.json`; its
generated table is unavailable at that worktree-relative location. Its 60
classes/691 explicit methods/3,004 inherited defaults/43 registration rows
must not be confused with the concurrent main-tree snapshot or binary behavior.

The next useful work is a family-by-family contract matrix: constructor
invariants, returned object shapes, asynchronous success/error completion,
callback ordering, Binder death/reconnect, capability reporting and settings
intent outcomes. Run those fixtures in isolated processes against exact build
identities, including strict-services when appropriate. This audit started no
user apps, entered no account data, attached no debugger and restarted no device.
