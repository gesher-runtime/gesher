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
packages, addons and nonpersistent packages are rejected. An ordinary authenticator
can be removed while it owns accounts. Unsupported keep-data, system-update,
archive and multi-user flags fail
explicitly. A caller cannot approve a removal through an intent extra or Binder
permission-result method.

Before mutation, an original-code retirement barrier blocks new component work and
account/capability captures. Server-local preflight checks run before force-stop or
revocation. Client account visibility and token grants, interactive sessions,
PendingIntents, install sessions/callbacks, URI grants, alarms, persisted jobs,
runtime grants and notification access are retired. Other providers' account
records and credentials remain intact. Allocated package UIDs are not recycled.

Code and internal/external app-specific data move to a private, journaled quarantine
before atomic registry removal. A failed registry write restores those paths and
does not emit removal events. An incomplete rollback blocks later publications
until recovery succeeds. Surviving journals reserve their original code generation,
so reinstall cannot cause old committed data to be restored. Cleanup does not follow
links. Locked committed remnants remain private for a later cleanup attempt; a
new installation receives fresh app data.

## Authenticator records and sync work

For the runtime's package-owned account model, uninstall stages a complete
DPAPI-protected account snapshot with that provider's accounts removed. Passwords,
user data, cached tokens, grants and visibility belong to those account records.
The same transaction stages sync settings without the removed accounts' automatic,
syncable and periodic entries. It serializes the shared snapshots and registry
commit under the sync-then-account monitor order; moving potentially locked app
paths and dispatching application callbacks happen outside those service monitors.

The package registry remains the commit point. A rejected registry write restores
app paths and leaves the provider's account records and sync settings intact. Once
the registry commits, memory retires the accounts and both shared files roll
forward from the private journal. Boot and install recovery publish any remaining
replacements before loading or reinstalling packages. A replacement is moved, not
copied: recovery cannot replay a consumed snapshot over newer data. New account and
sync writes first finish any pending publication. A failed package metadata cleanup
keeps the original-code retirement barrier until recovery also retires the package
from memory.

Active and queued sync operations carry an account incarnation. Removing and
recreating the same account name does not authorize an old completion, queued
request or already-dispatched periodic callback. The lifecycle executor cancels
active adapter calls and releases their bindings. Unrelated accounts, settings,
backoff and bindings remain available. Legacy delimiter-based sync settings migrate
to encoded endpoint identities before cleanup, including distinct accounts that
previously shared an ambiguous serialized key.

If an adapter loses account access while removal is pending, already admitted sync
work terminates through normal cancellation and binding cleanup. A failed uninstall
can therefore cancel transient work while preserving the account and persistent
sync settings; fresh authorized requests can run after rollback. Access revocation
also returns normally from periodic request callbacks, allowing later ticks to run.
Public caller authorization failures retain their existing exception behavior.

The reference is Android 16's
[AccountManagerService.validateAccountsInternal](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/accounts/AccountManagerService.java):
obsolete authenticators trigger account database/cache cleanup and targeted change
notifications. Android also retains credentials when a verified shared UID still
has installed members. That shared-UID account continuation requires integration
with the authenticated membership introduced by open PR #70. A blanket legacy UID
or an unverified manifest declaration is not sufficient evidence of that identity.
This change implements package-owned retirement and does not claim that separate
shared-UID continuation contract. Integrating #70 must reconcile its identity
transaction with this commit boundary and preserve any qualifying encrypted orphan
records without granting access to a replacement package merely by its name.

`AuthenticatorUninstallTest` exercises real Installer, account, sync and journal
paths with constructed package metadata and an explicit account-serialization seam.
Its host runner covers commit, staged-store and registry failures, partial metadata
publication, restart recovery, late callbacks and same-signer reinstall. The sync
contract additionally checks active/pending/periodic cancellation and fresh account
generations. The Windows service track runs the same fixtures with real framework
natives; these synthetic tests do not validate DPAPI encryption or a reference
Android device.

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
