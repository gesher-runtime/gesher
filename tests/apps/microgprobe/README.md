# Attached microG security probe

This is an actual installed APK declaring `INTERNET` and `POST_NOTIFICATIONS`.
Run it with the current `devsvc.apk` in an isolated Gesher test profile with
protected microG mode enabled (`GESHER_MICROG_SPOOF=1`). POST must initially be
denied. The base offline runner installs no GMS APK and needs no Google network
access. All credentials are test fixtures.

For the assembled runtime and both APKs, use
`powershell -File tests/windows/run-microg-security.ps1 -Dir <runtime-dir>`.
See [the integration guide](../../../docs/microg.md#build-and-offline-validation)
for build inputs and optional metadata suites. The manual sequence is:

1. Install both `devsvc.apk` and `microgprobe.apk` through the normal installer.
2. Launch `com.gesher.devsvc/.AccountActivity` with `--ez prepare-probe true`.
   The owner creates a hidden fake account and a visible fake account without a
   token grant, saves the authenticator request count, and creates a known
   PendingIntent capability with original extras.
3. Launch `com.gesher.microgprobe/.ProbeActivity` normally. Its process must have
   the assigned unprivileged package UID. Require `microgprobe: all passed`.
4. Launch the owner AccountActivity with `--ez verify-probe true`. Require the
   unchanged authenticator-count/secret and PendingIntent-capability checks.
5. AccountActivity's `--ez verify-retained-account true` also checks protected
   persistence after restart and removes the known test accounts.

To test explicit normal-token authorization without automating the native consent
dialog, run the guest `AccountSecurityTest grant-probe true` from the trusted host
control plane (shell Binder UID 2000), then launch ProbeActivity with
`--ez expect-token true`. Require its explicit-grant pass. Revoke with
`AccountSecurityTest grant-probe false` and rerun the normal negative probe;
visibility and an existing cached token must not reauthorize credentials. The
helper accepts an optional token type argument for testing an uncached scope.

The probe exercises real Binder calls for hidden account enumeration and
own-package visibility, cross-owner secrets/mutations, visible-but-ungranted
tokens, caller package forgery, protected C2DM broadcasts, receiver registration,
PendingIntent creator spoofing, non-exported activities/services, stopping an
installed private owner service, cross-package force-stop, own/spoofed location
requests without location permission, and host control. Provider/URI and
notification helpers also check cross-owner access, denied POST content and
unapproved listener access. The owner confirms denied token requests never
invoked its authenticator.

Content probes reject private-URI observer registration, forged/unauthorized
`notifyChange` and unpermitted sync-setting reads/mutations. The service checks
observer read access, notifier write access and the original notifier's identity
for deferred local sync.

`JobProbe` checks foreign service schedule/enqueue, `scheduleAsPackage`, global
administration and claimed package/user access. It exercises package-scoped
queries/cancellation and namespace isolation, then enqueues legitimate work for
its private `ProbeJobService`. The positive case requires
`PASS jobs own JobService ran and completed authenticated work`; compilation
alone does not establish that result. The guest `JobSecurityTest` separately
checks a seeded victim job remains untouched, unknown PID/UID rejection,
callback ownership and replacement of code during a pending service binding.

`PermissionCacheActivity` separately keeps the same app process alive while the
host grants and revokes POST. The runner requires `READY_INITIAL_DENIED`,
`GRANT_OBSERVED` and `PASS deny-grant-deny in same process` for that PID. It checks
both `checkSelfPermission` and `areNotificationsEnabled` without driving a native
permission prompt; the ordinary negative probe runs with POST denied.

Positive token delegation, cache/invalidation, custom-token expiry, interactive
authenticator continuation and opaque add/update sessions are exercised by the
owner fixture's normal AccountActivity flow. Native credential-consent dialogs
are not auto-approved by these tests.
