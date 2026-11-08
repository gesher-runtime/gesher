# Audio focus ownership, registration and callback lifecycle

Baseline: `6c0e1976456e6214763c7bddf58229eb3d4f3f66`. This follow-up
repairs the ordinary audio-focus stack in `AudioService`, including real
ownership changes and dispatcher calls. It does not provide an external audio
policy, telephony focus locking, automatic player ducking or fading.

## Confirmed defects and resulting behavior

The old implementation removed existing owners and dispatched permanent losses
before registering the new owner's Binder death recipient. A failed
`linkToDeath` therefore returned `AUDIOFOCUS_REQUEST_FAILED` after destroying
the existing owner. The focused regression reproduces that failure against the
unchanged baseline service using the actual compiled framework classes:

```text
java.lang.AssertionError: Failed new request changed the existing focus owner
```

Registration now precedes any stack mutation. A failed link, a dead Binder,
death during registration, or failed lifetime confirmation leaves existing
ownership and its callback stream intact. Successfully registered candidates
are retired and unlinked when they are not published. This follows the
[pinned MediaFocusControl request sequence](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/audio/MediaFocusControl.java),
with an additional explicit record-generation check for the interval between
linking and publication.

| Operation | Implemented behavior |
| --- | --- |
| The top client repeats the same gain and flags | Link and validate the incoming token, then discard that candidate and preserve the original token and dispatcher. This is the deliberate AOSP no-op rule, including when the new call supplies a different dispatcher. |
| The top client changes gain or flags | Retire its previous record, publish the new token/dispatcher and apply the new gain's losses to lower owners. The previous implementation only changed an integer and retained obsolete callback/lifetime state. |
| A client already below the top requests again | Retire its previous registration before adding its new request; do not emit a spurious restoration of the displaced top. |
| An active token dies | Remove all records using that Binder before restoring the surviving top. AudioManager shares one lifetime token among its listeners. |
| A retired recipient fires late | Its exact registration identity cannot retire a later record, even when the client ID or Binder object has been reused. |
| The current owner abandons or dies | Restore the surviving top with `AUDIOFOCUS_GAIN`, while `getCurrentAudioFocus()` continues to report that owner's original requested gain type. |
| A client is unregistered | Remove and unlink it without a restoration notification, matching the separate upstream unregister path. |

Both the no-op and replacement paths are compared with
`MediaFocusControl.requestAudioFocus`; shared-token removal and restoration
follow `removeFocusStackEntryOnDeath`, `notifyTopOfAudioFocusStack` and
`unregisterAudioFocusClient` in the same pinned source. The generation guard
also rejects late Binder-death notifications from registrations that have
already been unlinked. Committed focus loss/gain events remain deliverable
in their original order after retirement; the guard does not cancel them.

Loss severity now follows
[FocusRequester.focusLossForGainRequest](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/audio/FocusRequester.java):

| New request | No previous loss | Already ducked | Already transiently lost |
| --- | --- | --- | --- |
| `GAIN_TRANSIENT_MAY_DUCK` | Duck loss | No duplicate | Keep transient loss |
| `GAIN_TRANSIENT` or `GAIN_TRANSIENT_EXCLUSIVE` | Transient loss | Escalate to transient loss | No duplicate |
| `GAIN` | Permanent loss | Escalate to permanent loss | Escalate to permanent loss |

Permanent losers are removed and unlinked. In particular, a later transient
request no longer leaves an earlier ducked client believing that it may
continue ducked playback. Restoration always uses `GAIN`, including for
clients whose original request was transient, duckable or exclusive; the old
code incorrectly sent their original request type as the callback.

## Callback ordering and bounded state

[IAudioFocusDispatcher is a oneway interface](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/IAudioFocusDispatcher.aidl).
The service submits callbacks to one private executor while holding the state
lock, waits for the corresponding publication to finish, and invokes the
dispatcher outside that lock. This preserves committed event order for local
dispatchers as well as remote clients. A listener may reenter from another
thread without deadlocking the focus stack. A blocked listener does not block
subsequent state changes. Remote and local runtime callback failures do not
interrupt retirement or stop delivery to later listeners.

The initial successful grant is the request's return value; it does not
invent an initial gain callback. A null dispatcher remains valid, and token
lifetime/state changes still occur. Duck notifications remain delivered to
the app because there is no player-ducking backend to perform that action on
its behalf.

The stack also retains AOSP's precise bound: `MAX_STACK_SIZE = 100` with the
source's `size > MAX_STACK_SIZE` entry check. Thus 101 transient records may
be retained; the next request fails before registration or mutation. The
test checks this boundary and the source's same-client behavior when that
entry check has already been reached.

Ordinary `DELAY_OK` and `PAUSES_ON_DUCKABLE_LOSS` properties are retained as
part of request identity. With no locked focus owner, accepting delay does
not itself delay a grant. Locked-focus requests cannot be honored by an
unregistered external policy and are rejected without changing state.
Malformed/null requests and unknown unsupported flags also leave state
unchanged. The ordinary Binder entry rejects `AUDIOFOCUS_FLAG_TEST` with
`IllegalArgumentException`, as in
[pinned AudioService.requestAudioFocus](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/audio/AudioService.java).
The public request builder's separate gain validation is tested in the
Windows entry point.

## Regression evidence and reproducibility

`tools/test_audio_focus.py` compiles the changed production service with the
real Android framework/core classes and runs the common portion of
`AudioFocusLifecycleTest`. The test controls only Binder lifetime and the
host JNI boundaries needed to construct those classes. It does not replace
the focus stack, executor, dispatcher implementation or framework constants.
`tests/audio/framework_host.c` uses the existing host native adapter, plus
AudioSystem's unrelated constructor volume initialization and static format
limits; those are not an audio backend test.

The cases cover rejected registration and unchanged state; same-client no-op
and changed gain/flags; replacement and lower-stack reacquisition; all four
gain types; duck-to-transient and permanent escalation; no loss downgrade or
duplicate notification; exact regain value; shared-token death; non-top
death; stale registration callbacks; unregister versus abandon; null
listeners; remote/runtime dispatcher errors; cross-thread reentry; callback
ordering while a listener is blocked; and bounded-stack cleanup.

Example invocation, supplying the existing verified build inputs:

```sh
python tools/test_audio_focus.py \
  --java-home "$JDK_HOME" \
  --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$ANDROID_CORE_CLASSES" \
  --app-classes out/audit-continuation/gesher-app-classes.jar
```

Adding `--source-ref 6c0e1976456e6214763c7bddf58229eb3d4f3f66` compiles the
old service in the same harness. It fails the registration-state assertion
quoted above, before attempting any reflection on new implementation fields.

The full fixture's Windows main additionally constructs the generated
`IAudioServiceStub`, forces the real AIDL proxy through Binder/Parcel, and
uses a real `AudioManager`, `AudioFocusRequest`, listener map and Handler to
request, receive a duck loss, abandon and receive `GAIN`. It supplies a small
Context through AudioManager's test constructor so unrelated route discovery
is not required. The normal `getContext`/`setContext` path still executes.
The complete marker is `AudioFocusLifecycleTest all passed:`; a host-only
marker does not prove that the Binder/public-manager portion ran.

Local host execution and compilation are recorded separately from pending
Windows runtime execution. Final local results are **206 host assertions
passed** and successful JDK 21, target 17 compilation of both the production
service and complete fixture against the real Android core module/framework.
The unchanged-baseline negative control fails the registration-state
assertion above. Independent source/test review also checked the lifecycle
and corrected the fixture's Context initialization before freezing it.
No claim is made that this fixture exercises
cross-process ART death notification, Windows playback attenuation, or
telephony/external-policy enforcement. Those remain integration boundaries.

## Pinned source identity

All references above were downloaded from `android-16.0.0_r4` using Gitiles
`?format=TEXT` and decoded before inspection. SHA-256 values below apply to
the decoded source bytes.

| Source | SHA-256 |
| --- | --- |
| `MediaFocusControl.java` | `9d918ce9111fe4e4b08e57c8985fc31bd48f95584f58452bc7486e3b9e0593ca` |
| `FocusRequester.java` | `7fc4da65d566e38391b4d560cca143429da079d7109b38cd149af371eeaebb8b` |
| `AudioService.java` | `59d74a2df684f5995537f087e9b47b71013fcb42965b300041bb841e698af446` |
| `AudioManager.java` | `00f7fd83166aa55f650529594c10c8e8cec61718ae608c12a4521bcf6aaeb404` |
| `IAudioFocusDispatcher.aidl` | `dca8ad191bd84134119c675faadd332cf0a86211d86089a53d17a8e34ab2021e` |

The [pinned AudioManager source](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/AudioManager.java)
defines the shared `mICallBack`, request registration, dispatcher-to-Handler
delivery and public request/abandon path used by the fixture.

This patch does not implement Android's package/AppOps hardening, foreground
eligibility policy, multi-focus or virtual-device policy, external-policy
followers, delayed grants behind a locked owner, telephony handling,
automatic duck/fade/restore of audio players, or playback-configuration
inventory. The repaired ordinary focus ownership and notification lifecycle
must not be described as full Android audio-policy parity.
