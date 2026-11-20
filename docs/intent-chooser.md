# Intent chooser and sharing

`ACTION_CHOOSER` uses a system-server-owned Windows dialog to select an installed
Android activity. The dialog has no initial selection; Cancel is its default
button. Clicking Select launches the chosen component through the same activity
starter, caller permission checks, result routing, and URI-grant publication as
an ordinary Android activity start. There is no production first-row fallback.
Explicit app components and package-restricted intents retain normal application
routing. Chooser admission also enforces the caller's task-placement permission
and the runtime's supported user boundary.

## Contract and implementation

The contract references are the Android 16 IntentResolver implementation and the
public `Intent` documentation:

| Contract | Evidence | Gesher implementation |
| --- | --- | --- |
| Resolve the payload, filter inaccessible/excluded components, and resolve initial choices | [ResolverListAdapter](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/java/src/com/android/intentresolver/ResolverListAdapter.java), [ChooserRequest](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/java/src/com/android/intentresolver/data/model/ChooserRequest.kt) | `IntentChooser` captures the original caller and installed package generations; each offered row has an explicit resolved component. Disabled activities and components inaccessible to the caller do not become menu entries. |
| Component access is based on admitted UIDs, including shared UIDs | [ResolverListController.filterIneligibleActivities](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/java/src/com/android/intentresolver/ResolverListController.java), [ActivityManager.checkComponentPermission](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/core/java/android/app/ActivityManager.java) | Chooser filtering and normal activity startup use the same UID permission policy. Distinct packages with the same admitted UID retain component-owner access; unrelated UIDs must satisfy exported and permission requirements. |
| Resolve selectors and actual content-provider MIME types | [Intent.resolveTypeIfNeeded](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/core/java/android/content/Intent.java), [ApplicationPackageManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/core/java/android/app/ApplicationPackageManager.java), [ContentResolver.getType](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/core/java/android/content/ContentResolver.java) | Primary, initial and alternate choices resolve their effective selector and type while delivering the original outer payload. An explicit outer component takes precedence. Missing content MIME is obtained through the real framework resolver and provider registry, never inferred from a filename or filled with a default. |
| The chooser can resolve protected MIME metadata without granting payload access | [IntentResolver manifest](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/AndroidManifest-lib.xml), [ContentProviderHelper.getMimeTypeFilterAsync](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/services/core/java/com/android/server/am/ContentProviderHelper.java), [ContentProvider.Transport](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/core/java/android/content/ContentProvider.java) | Android's resolver requests `GET_ANY_PROVIDER_TYPE`; full MIME is available to that permission and to the system UID. Gesher's internal chooser resolver uses platform authority for MIME metadata only. Candidate activity access and selected-target URI grants continue to use the original creator. |
| At most two initial intents precede app suggestions | [`EXTRA_INITIAL_INTENTS`](https://developer.android.com/reference/android/content/Intent#EXTRA_INITIAL_INTENTS) | Only the first two initial intents are considered. Unresolved initial actions are omitted. `LabeledIntent` text labels are retained. |
| Auto-launch requires one unsuspended option and caller permission to auto-launch | [ChooserActivity.shouldAutoLaunchSingleChoice](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/java/src/com/android/intentresolver/ChooserActivity.java), [ResolverActivity](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/java/src/com/android/intentresolver/ResolverActivity.java) | `EXTRA_AUTO_LAUNCH_SINGLE_CHOICE=false` always opens the chooser. Suspended rows remain visible and cannot be selected. |
| Forward the activity result through the chooser | [DisplayResolveInfo.createResolvedIntent](https://android.googlesource.com/platform/packages/modules/IntentResolver/+/refs/tags/android-16.0.0_r1/java/src/com/android/intentresolver/chooser/DisplayResolveInfo.java) | The selected activity retains the original result recipient/request. The chooser adds the resolver forwarding flags and carries the result route explicitly, since the host dialog has no application ActivityRecord. |
| Notify a chosen component only after a successful start | [`EXTRA_CHOSEN_COMPONENT_INTENT_SENDER`](https://developer.android.com/reference/android/content/Intent#EXTRA_CHOSEN_COMPONENT_INTENT_SENDER), [`ChooserResult`](https://developer.android.com/reference/android/service/chooser/ChooserResult) | The supplied PendingIntent receives `EXTRA_CHOSEN_COMPONENT` and `EXTRA_CHOOSER_RESULT` after normal launch admission succeeds. Cancel, stale choices and rejected grants send no selection callback. Callback failure does not undo an accepted target launch. |
| Preserve share content and grant semantics | [Intent.migrateExtraStreamToClipData](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r1/core/java/android/content/Intent.java) | The actual framework migrates SEND/SEND_MULTIPLE streams and text/HTML to ClipData, using the real caller's application metadata. Its implicit-grant policy remains in force. Wrapper data/type or unrelated activity flags cannot replace the nested target. |

The dialog waits for the covered task-top activity's pause acknowledgment, with
the same bounded 500 ms ordering window used by other host UI. The result
recipient can be an older stopped activity: closing the chooser resumes the
covered top and keeps that recipient's result queued. A late pause acknowledgment
cannot strand the caller after cancellation. `NEW_TASK` cancels the original
result dependency once; `NO_USER_ACTION` controls the pause's user-leaving bit.

The original caller process, result activity, code generation and covered activity
are retained while the dialog is pending. Activity/process death, package
replacement/removal, a hidden task or a replacement task top invalidates the
pending UI. The native modal loop checks that lifetime on its own JNI thread and
closes when it expires. Before publishing the target, the starter checks caller
and target generations again. Once a target is accepted, its lifecycle and grants
belong to its normal ActivityRecord. Chooser cleanup removes its reservation and
releases its pause coverage exactly once.

An accepted chooser reservation also keeps the runtime busy until its worker has
closed the native window and completed cleanup. This includes `NEW_TASK` requests
from a process with no ActivityRecord. Caller retirement makes the window's
lifetime predicate false, but does not make the device idle while that window is
still closing. Admission clears the old idle interval; final cleanup starts a
fresh interval when no activity or other chooser remains, even if the idle
monitor never polled during the operation. A selected activity then owns its
ordinary busy lifetime. Backend failure and cancellation release the same
reservation without creating a synthetic activity.

MIME resolution uses a private platform-attributed resolver with the chooser's
privileged metadata authority. The real `ContentResolver` and provider transport
perform `getTypeAsync` and its timeout/error handling. This can obtain a protected
provider's full type even when the creator cannot acquire that provider. The
resolver and provider handle never leave the server. The creator process owns
only a temporary counted provider reference, released before the dialog opens;
it receives no provider handle or URI grant. Ordinary provider acquisition is
unchanged, and the chosen activity's permissions and URI payload/grant checks
retain the original creator's authority. Provider callbacks occur before chooser
lifecycle admission, outside the ordinary launch and package metadata monitors.
Each resulting option captures its resolved metadata and package generation
atomically, and the creator generation is retained across the provider call.

The shared native choice controls live in `compat/credentials/account_chooser.inc`.
The existing account JNI entry remains available with its original 64-row bound.
The intent entry accepts at most 256 rows and an enabled mask; its memory and JNI
references are local to the modal call and released on every exit. Its native
availability check requires the trusted server role and a visible Windows window
station. A missing backend returns `START_INTENT_NOT_RESOLVED`; failure or closure
after admission cancels the operation without launching a target.

## Supported scope and unavailable operations

The chooser supports ordinary installed Android activities, initial and alternate
payloads, exclusions, package replacement extras, result forwarding, and chosen
component callbacks. The original target's URI access is checked for the selected
package at launch; merely showing its row grants nothing. Grant-mode support is
provided by the runtime's URI-grant service, including its explicit unsupported
errors.

Refinement callbacks, direct-share targets, chooser custom actions and modify-share
actions require further contracts. A request using one of these nonempty features
returns `START_INTENT_NOT_RESOLVED`. Empty or null optional arrays do not request a
feature. No requested application step is silently skipped. Native Windows share
applications and the external browser adapter have no Android activity/result/grant
owner and are not offered as installed Android targets.

## Validation

`tools/test_intent_chooser.py` runs the current production ATM, chooser, package,
URI-grant and PendingIntent code against real framework classes. Its fixture
constructs isolated package metadata and scripts only native user input. It checks
that the actual selected ActivityRecord is published, real launch/resume/pause
transactions are emitted, results reach the original caller, grants are exclusive
to the selected target and revoked on finish, and PendingIntent callbacks are
suppressed for cancellation and failed admission. It also checks malformed input,
exclusions, unresolved/capped initials, single-choice preferences, suspended rows,
task flags, late pause acknowledgment, hidden tasks, code replacement, process and
activity death, and retirement during final publication.
Activity-free chooser tests also cover idle shutdown ownership through selection,
cancellation, native open failure, exceptions, caller retirement and concurrent
choosers, including a complete operation between idle-monitor polls.
The same suite covers same-UID private/protected components and unrelated-UID
denial, selectors in primary/initial/alternate payloads, provider-backed MIME
resolution, explicit type precedence, unknown/error responses, protected full MIME
metadata, and acquisition/release of a real counted provider connection. A caller
without provider access can discover matching rows but cannot transfer the
protected URI. The provider's data and context are isolated test fixtures; the
framework `ContentProvider.Transport` executes its actual MIME permission and
callback path, and resolver, provider registry, activity admission and URI grants
remain production code.

```sh
python3 tools/test_intent_chooser.py \
  --java-home "$JAVA_HOME" \
  --framework-classes /path/to/framework-classes \
  --core-classes /path/to/core-classes \
  --app-classes /path/to/gesher-app-classes.jar \
  --apksig /path/to/apksig.jar
```

The host adapts only native initialization/identity, clock, disabled tracing,
PendingIntent Parcel size accounting, and unrelated framework locale/cache storage. It is not a Windows
rendering or ART transport result. `ChooserContractTest` is registered in
`tests/windows/tracks/95-browser.ps1`; on Windows it uses actual AIDL/Parcel calls
and the same production-path assertions.

`tests/windows/run-account-chooser-native.ps1`, called by the existing microG
security suite, now runs 16 account/intent modes on private, never-switched Windows
desktops. The actual JNI/Win32 dialog is driven by its own native fixture, including
no default selection, selecting the second row, explicit cancellation, disabled
rows, lifetime expiration/exceptions, invalid inputs and non-server rejection.
These fixtures are separate from the application jar and contain no real accounts
or user payloads. Cross-compilation of the DLL and native driver is required for
both x86-64 and ARM64; execution of the Windows suites is a separate validation
step.

### Detached overlapping chooser coverage

A no-result `NEW_TASK` chooser selects the focused task even while an earlier
native chooser already covers that task. Focus and eligibility to resume are
separate decisions: the second window acquires its own coverage before either
worker can complete. Closing one chooser cannot resume the covered activity or
release the other chooser's real native reservation. A task with no attached
process retains the existing detached admission behavior. Caller authority,
selection validity and normal cancellation remain unchanged.

`ChooserContractTest` exercises two simultaneous detached choosers from different
authenticated caller processes. The first worker must return without resuming
the shared covered source; the last completion resumes it once. The same probe
fails published `39a1cc15928048e49d18af320f1574f38f5669e7` before this change.
The fixture controls host user input and process metadata and does not claim
Windows UI, ART or remote Binder execution.
