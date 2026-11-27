# Legacy device administration on Windows

Gesher handles `DevicePolicyManager.ACTION_ADD_DEVICE_ADMIN` through a native
consent window. It identifies the administrator receiver, displays the app's
explanation and the authority being requested, and lets the user reject the
request. Closing or rejecting the window leaves the app inactive and returns
`RESULT_CANCELED`. Acceptance returns `RESULT_OK` only after the grant is saved
for that installation. No device or profile owner is created.

Currently supported administrators must request exactly the legacy `force-lock`
policy. A valid receiver must require `android.permission.BIND_DEVICE_ADMIN`,
handle `DEVICE_ADMIN_ENABLED`, and supply valid `android.app.device_admin` XML.
Other policy sets are refused with an explanation rather than being recorded as
granted. No Android device-admin or managed-user feature is advertised as a
result of this limited implementation.

`lockNow()` with flags zero locks the current Windows session using the real
`LockWorkStation` API. The server checks the attached application's current
installed identity and its approved receiver policy before the operation.
Windows initially acknowledges only submission; Gesher waits up to three seconds
for the session's real locked state and throws if it is not confirmed. Parent
profile locks and credential-encryption flags are unsupported. The device's
password, camera, encryption, wipe, owner, and permission policies are not granted.
Automatic maximum-time-to-lock policy is unsupported and throws rather than
silently succeeding.

Grants are written atomically to `system/device-admin-grants`, bound to the
installer's per-installation owner UUID. Reinstalling a package cannot inherit
its previous administrator grant. A pending consent request cannot activate
replacement code. Standalone services use transient grants. Active administrators
block ordinary uninstall until deactivation; applications can remove their own
administrator through `removeActiveAdmin`. The trusted Control Center also offers
deactivation so the user does not depend on the app to revoke its authority.
In an application's details, **Revoke device administrator access** removes all
of that application's enrolled receivers. The button waits for the authenticated
host command's answer and keeps retry access when revocation fails.
The same host-only control is available as `gesher-app device-admin PACKAGE`
(query) or `gesher-app device-admin PACKAGE revoke`. It cannot grant authority.
Enabled and disabled broadcasts are dispatched after committed state changes.

This is not a complete enterprise policy controller. Device/profile-owner
provisioning, arbitrary policy sets, and the Settings remove-warning query remain
unsupported. Existing unmanaged owner/encryption answers remain accurate.

## Contract and incident evidence

Android's [ACTION_ADD_DEVICE_ADMIN contract](https://developer.android.com/reference/android/app/admin/DevicePolicyManager#ACTION_ADD_DEVICE_ADMIN)
requires a user flow for granting or rejecting the named receiver, including
renewed consent for newly requested policies. The
[legacy device administration guide](https://developer.android.com/work/device-admin)
describes receiver metadata and user activation. Microsoft's
[LockWorkStation contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-lockworkstation)
explicitly distinguishes a submitted asynchronous request from a locked session;
Gesher's existing WTS session query is used for confirmation.

The reported FaceLock Guard 7.6 APK (version code 56, target SDK 34, SHA-256
`06bfa36a5dc8be4adcaaf5756bda71a29d025c1c3d93f23688b95586a65ec063`)
declares an exported `AdminReceiver` protected by `BIND_DEVICE_ADMIN`; its
`res/pO.xml` contains only `<uses-policies><force-lock/></uses-policies>`.
The crash event `5fa0-e2a55f994ad4` was an uncaught `ActivityNotFoundException`
for `android.app.action.ADD_DEVICE_ADMIN`, rather than a native fault.

## Validation limits

`tools/test_device_admin_store.py --java-home JAVA_HOME` compiles the production
grant store and exercises restart, revocation, malformed input, transient
isolation, and failed-write behavior. `tools/test_device_admin_requests.py`
exercises the production request queue and resolution with explicitly isolated
Android, package-service and dialog boundaries.

`DeviceAdminPolicyRuntimeTest` runs on Windows/ART using real signed APKs,
PackageManager and AssetManager XML parsing. It checks supported and unsupported
policies, activation, current install identity, callbacks, revoked/foreign
callers, uninstall protection, missing host support, rejected host locks, and an
APK update adding unsupported policies. Its host availability/lock and attached
caller resolver are explicit test boundaries: it never locks the workstation.
`tools/test_device_admin_runtime.py` builds the metadata fixtures and can run the
test in an isolated profile; CI builds these fixtures in `framework.yml` and
executes the test in Windows track 99.

The native `device_admin_dialog_test` executes the production consent controls
on a private desktop, including activation, rejection, closing, Enter/Escape,
actual Hebrew text and unsupported policy decisions. Control Center WPF tests
exercise asynchronous revocation, failure/retry, and the host's returned state;
`SpecialAccessTest` checks the actual host Binder transaction's authorization.
These checks do not establish a successful physical Windows session lock or
FaceLock's complete workflow. The installed user profile is never used as a
test fixture.

`ExactAlarmContractTest settings-lifecycle` exercises the shared production ATM
Settings lifecycle, including administrator enrollment cancellation, ordered
pause/stop, delayed results and resume. The default test invocation still runs
the full alarm and Settings suite.

The native backend has a dedicated availability probe, so an older runtime
without the lock implementation cannot activate an administrator merely because
its clipboard backend exists. Grants are valid only while the current receiver
still requests the exact supported policy set.
