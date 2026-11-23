# Provider death reports and client cleanup

This addresses the real Binder signature
`IActivityManager.unstableProviderDied(IBinder)` exposed by issue #56's descriptor
inventory. The similarly named `IContentProvider` method in the anonymous
`ContentResolver` inside `ActivityManagerService` is not this service method.

## Application-visible behavior

Previously, the service inherited an empty `IActivityManager.Default` method. A
client that caught `DeadObjectException` could report the provider but immediately
reacquire its stale publication until ordinary process-death notification arrived.
Ordinary provider-process cleanup also discarded connection records without
invalidating unstable clients or retiring stable dependents.

The service now delegates the actual Binder method to `ContentProviders`. It
validates the connection type and the attached caller's UID, process record and
installed package generation. Null and unrelated Binder values produce the pinned
Android exceptions; retired registered connections are harmless repeated reports.
A provider's real `IBinder.pingBinder()` runs without the provider, ATM or package
monitors. A provider that answers remains alive regardless of a client's claim.

After a failed probe, retirement rechecks the connection, caller, publication,
owner package and current process incarnation. An intervening release,
republication, replacement or completed death cannot retire a newer lifetime. The
existing ATM-to-provider lock order makes this commit atomic with new admission.
The owner is marked dead, rather than intentionally stopped, so existing sticky
service recovery semantics remain available. The normal process-death path runs
with server identity after releasing both monitors.

Both early reports and ordinary process-death notification remove the lost
connections before notifying clients. Stable dependencies use the existing
generation-checked dependency-death path, including its system/persistent-process
exceptions. Unstable clients receive the old provider Binder through
`IApplicationThread.unstableProviderDied`; their processes remain available to
acquire a replacement. All client callbacks occur without lifecycle monitors.
Repeated or concurrent notifications are idempotent.

## Android reference

The contract was checked against AOSP `android-16.0.0_r4`:

- [ContentProviderHelper](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/am/ContentProviderHelper.java):
  `unstableProviderDied` validates the token, probes liveness outside the service
  lock, rechecks the publication and invokes normal process death;
  `removeDyingProviderLocked` distinguishes stable from unstable dependents.
- [ActivityManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/am/ActivityManagerService.java):
  the Binder entry point delegates to that provider controller.
- [ActivityThread](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ActivityThread.java):
  `handleUnstableProviderDiedLocked` discards the cached provider and reports its
  connection before the client retries acquisition.

Decoded upstream source SHA-256 values used for the review:

| File | SHA-256 |
| --- | --- |
| ContentProviderHelper.java | `dec65b94d9669541b29b97099fdaaff3cfe59472075ea0ef65c93aa583ad5b2e` |
| ActivityManagerService.java | `2df3c48c2e8769bf00193c70465e6b85d5208c48e23eac62820c9c083ae95601` |
| ActivityThread.java | `394e70067e4b4b675f7d38f924137837cf01daf062422217084b495e0efbec7a` |

The implementation reuses Gesher's authenticated process/connection records and
Windows process-generation retirement. It does not import AOSP's process manager
or add application/package-specific exceptions.

## Regression evidence and execution limits

`PackageEnabledLifecycleFixture` now exercises the actual AM method through the
generated `IActivityManager` proxy when its Binder mode is enabled. The existing
Windows track 99 and framework host gate run the extended `all` group. The
focused host command is:

```sh
python tools/test_package_enabled_lifecycle.py \
  --java-home "$JAVA_HOME" \
  --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" \
  --apksig "$APKSIG_JAR" \
  --group provider-death
```

Coverage includes a live provider, null/unrelated/foreign-UID/foreign-process
tokens, a reused caller PID, publication and owner/client generation changes
during the probe, release during the probe, reentrant and concurrent death,
stable versus unstable cleanup, duplicate physical death and fresh acquisition.
The same source compiles against the pre-fix application classes and fails at
`Reported dead provider remained the current process`.

The host fixture executes real PM/AM/controller code with constructed package
metadata, controlled test-only provider Binders and recording application threads.
Its classloader is separate from production classes, mirroring ART's runtime
package boundary. It does not execute ART, a remote application process, native
Windows process termination or an Android device differential. Windows CI can
exercise generated AIDL/Parcel dispatch on ART; its recording fixture still does
not establish real multi-process recovery. These limits do not complete the broad
A03 or provider compatibility roadmap.
