# User-confirmed package removal

Gesher's PackageInstaller uninstall entry point and legacy
`PackageManager.deletePackage` / `IPackageManager.deletePackageAsUser` adapter are a deliberate, user-mediated
extension of Android's permission-gated API. Any authenticated, current application
may request removal of an ordinary installed APK. Actual authority comes from a
fresh host dialog, whose default is **No**. This does not grant `DELETE_PACKAGES`,
change permission queries, recognize a particular store, or permit silent deletion.
The original process generation, caller code, target code and requested version
are checked again after approval.

The legacy route derives the requesting package from its attached Binder caller;
it forwards the original user, version and flags to the same broker. Its
`IPackageDeleteObserver` receives the corresponding success, cancellation or
failure code. Neither route returns silently from an unimplemented deletion stub.

The supported operation is complete removal for user 0 with flags 0. System
packages, addons and nonpersistent packages are rejected. Removing an authenticator
also retires the accounts it owns and their sync work after registry commit.
Unsupported keep-data, system-update, archive and multi-user flags fail
explicitly. A caller cannot approve a removal through an intent extra or Binder
permission-result method.

Before mutation, an original-code retirement barrier blocks new component work and
account/capability captures. Server-local preflight checks run before force-stop or
revocation. Client account visibility and token grants, interactive sessions,
PendingIntents, install sessions/callbacks, URI grants, alarms, persisted jobs,
runtime grants and notification access are retired. Other providers' account
records and credentials remain intact. Allocated package UIDs are not recycled.

An authenticator's account-store marker is written before registry mutation. The
authoritative installed-package registry decides whether that retirement committed
or rolled back; a failed package scan or disabled authenticator is not a removal.
Committed account retirement cancels running, pending and periodic sync work and
persists its settings cleanup before deleting the protected account records. A
failed cleanup keeps those accounts sealed and is retried before a reinstall can
be published. Registry rollback keeps the original owner's accounts.

Account-removal events capture each authorized recipient's code identity and
removal generation while the account is still authoritative. Broadcast reservation
and deferred delivery recheck that lifetime, so an old event cannot disclose the
account name to a same-signer reinstall. A safe update in the same installation
keeps its event. A serial notification worker carries only account/recipient
metadata; queued work does not retain passwords, tokens or credential entries.

The account and sync host contract gates exercise persistence failure/retry,
periodic cancellation, delayed delivery, same-signer reinstall, a reinstall during
broadcast handoff and bounded worker fan-out. They use actual production services
with declared package, protected-storage and JNI fixtures; they do not qualify
Windows DPAPI, ART or cross-process Binder execution.

Code and internal/external app-specific data move to a private, journaled quarantine
before atomic registry removal. A failed registry write restores those paths and
does not emit removal events. An incomplete rollback blocks later publications
until recovery succeeds. Surviving journals reserve their original code generation,
so reinstall cannot cause old committed data to be restored. Cleanup does not follow
links. Locked committed remnants remain private for a later cleanup attempt; a
new installation receives fresh app data.

If storage or registry commit fails after approval and revocation, restored app
files do not restore the revoked capabilities. The still-installed app may need
fresh permission or account consent. Declining the host dialog itself does not
start this mutation.

Only committed removal sends a successful status callback and protected
`PACKAGE_REMOVED` / `PACKAGE_FULLY_REMOVED` events with the real package URI and UID.
The native callback, storage recovery and same-signer reinstall regressions use
synthetic packages and isolated profiles. Real Store UI removal remains unverified:
the user requested all real profiles reset, so final adapter qualification used
isolated synthetic packages.
