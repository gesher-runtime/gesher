# Explicit URI grants — 2026-11-08

This follow-up starts from PR #8 commit
`6c0e1976456e6214763c7bddf58229eb3d4f3f66` and compares the explicit
`Context.grantUriPermission` / `revokeUriPermission` path with the pinned
`android-16.0.0_r4` implementation. It changes the existing grant store and
provider-policy lookup; it does not add automatic Intent grants or a chooser.

## Confirmed application failures

The real `ContextImpl` wrappers call ActivityManager with their application
thread and URI. Gesher already authenticates that thread and keeps explicit
read/write grants, but several decisions differed from the pinned service:

- An app with a provider's declared read permission could read a document but
  could not explicitly grant another app read access. `UriGrants.grant` checked
  only provider ownership or an existing URI grant, discarding the caller's
  actual general/path permission. The valid delegation threw `SecurityException`.
- A recipient could not relinquish its own explicit grant through
  `revokeUriPermission`; only the provider owner could revoke anything. Android
  also permits direct-authorized callers to revoke matching grants and permits
  ordinary recipients to relinquish their own grants.
- Exact grant normalization erased query and fragment values. Granting
  `content://authority/item?record=1#part` consequently authorized a distinct
  query/fragment at the same path. Android's `GrantUri` preserves those values.
- Provider lookup omitted `GET_URI_PERMISSION_PATTERNS`, so the real framework
  metadata projection removed the declared grant patterns. Pattern-only grants
  failed, and a broad grant flag could override a restrictive pattern list.
- Read/write bits were combined across different exact/prefix identities. The
  pinned service requires one matching grant identity to hold all requested
  modes; repeated grants to that same identity may accumulate modes.
- A basic request whose target already had direct access, or whose target or
  provider no longer existed, could throw instead of returning without creating
  a grant. The pinned service recognizes these outcomes after caller validation.

These defects are reproduced in the actual service methods with controlled
installed/provider state. No third-party APK crash is claimed from the host test.

## Implemented policy

| Operation | Resulting behavior |
| --- | --- |
| Direct delegation | Evaluate the actual provider's exported flag and the sender's declared general/path permissions for every requested mode. The provider owner retains its existing authority. |
| Explicit regrant | Require a current matching URI grant with every requested mode. An exact grant cannot become a prefix grant. Different grant identities cannot be combined to obtain stronger authority. |
| Provider grantability | Read the real parsed grant patterns. A nonempty pattern list restricts the operation even when the provider also declares `grantUriPermissions=true`. |
| Already-authorized target | For a basic grant, return without adding state when the target already has direct access. Prefix/persistable requests and forced dynamic-provider policy do not use that shortcut. |
| Unknown endpoint | After authenticating the sender and validating the arguments, a missing provider or target produces no grant and leaves existing grants unchanged. A null target remains invalid. Revocation of an absent provider is harmless. |
| Revocation by direct-authorized caller | Remove requested modes from matching grants for the specified target, or all targets when none is specified. All requested modes must be directly authorized for this wider operation. |
| Revocation by ordinary recipient | Remove only that recipient's own explicit grants. A request naming a different recipient cannot affect it. Relinquishing a grant does not revoke a distinct grant already delegated onward. |
| URI identity | Preserve query and fragment for exact grants, removing only the supported embedded user ID. Prefix grants and revocation continue to compare URI path segments. Revoking a descendant does not remove an ancestor prefix grant. |
| Installed generations | Check provider, sender and target against the current package inventory. Retiring packages cannot acquire usable grants; a replacement package object cannot inherit an old target/provider grant. |

The provider snapshot and installed generations are captured under the package
manager monitor. A held-grant authorization check and publication share the grant
monitor, preventing intervening revocation from being ignored. Lock ordering is
package manager, then grant store; caller authentication happens before those
monitors. Package-removal capability cleanup still uses the existing callback.

Gesher's legacy mode can assign the same numeric UID to unrelated packages. This
patch retains authenticated package ownership instead of treating that collision
as Android shared-user ownership. A specific regression verifies that equal
numeric UIDs do not allow another package to grant a private provider's URI.

For providers with `forceUriPermissions`, general manifest permission alone does
not substitute for the unavailable dynamic provider-side check. The actual owner
and explicit URI grants remain independently usable. No dynamic-permission
backend is represented as implemented.

## Executed evidence

```sh
python3 tools/test_uri_grants.py \
  --java-home /path/to/jdk21 \
  --framework-classes /path/to/framework-classes.zip \
  --core-classes /path/to/coremod \
  --app-classes /path/to/gesher-app-classes.jar \
  --apksig /path/to/apksig.jar
```

**The final host run passes 138 checks.** They exercise actual ActivityManager
grant/revoke/check methods, `UriGrants`, `ContentProviders` acquisition policy,
`PermissionPolicy`, framework `PackageImpl`/parsed-provider declarations,
provider-info projection, path matching and returned `UriPermission` values.
Successful delegation is followed by real target permission checks and provider
acquisition-policy checks; this is not a second implementation of the grant model.

Coverage includes direct and path-based delegation, read/write separation,
private providers, same-numeric-UID isolation, prefix delegation, exact query and
fragment neighbors, embedded user normalization, pattern precedence, mode
coalescing, self/target/global revocation, installed-generation replacement,
pending removal, capability cleanup, missing endpoints, malformed arguments,
unsupported persistence and wrong application-thread rejection. Rejected
operations are checked against the complete preexisting grant state.

The host envelope constructs actual parsed manifest objects rather than parsing
a binary APK. `Unsafe` omits unrelated package-installer Binder and activity
bootstrap constructors and supplies the attached process inventory. The actual
permission-policy constructor and custom declared permissions remain in use.
Framework resource loading is unavailable in the host JVM; its existing failure
path keeps unknown framework permissions denied, while the tests exercise real
custom permission declarations. Small native adapters provide initialization,
clock/logging and controlled thread-local Binder PID/UID. They implement neither
permission policy nor Binder transactions.

Six negative controls compile the pre-change URI/provider sources using
`--source-ref 6c0e1976456e6214763c7bddf58229eb3d4f3f66` and one of
`--case delegation`, `identity`, `modes`, `patterns`, `revocation`, or `validation`.
They fail respectively on rejected lawful delegation, leaked query-neighbor
access, combined authority from different grants, missing parsed patterns,
rejected recipient revocation and the missing-target outcome. The final positive
fixture passes, and `git diff --check` is clean.

## Windows/ART fixture and limits

The separate entry point is:

```text
gesher.services.ServiceFixtureRunner UriGrantContractTest
```

It takes no APK arguments. The wrapper creates the private writable
`ApplicationSharedMemory` region. The fixture loads the real runtime/framework
assets, creates an isolated data directory, enables the distinct-package identity
mode for its duration, and restores the environment and caller identities. Its
Binder branch uses the compile-checked `StandIn.of` factory in strict mode,
forces the generated `IActivityManager` proxy/Stub/Parcel route and then runs the
same contracts. Its expected final marker is:

```text
UriGrantContractTest all passed: 138 checks; binder=true
```

The local host result has `binder=false`. It is not Windows/ART execution,
cross-process transport, a real content-file open, binary manifest parsing, or a
third-party application result. The separate runtime fixture must pass its own
CI gate.

Automatic grants attached to Activity/Intent/ClipData delivery, activity/service
owner-token lifetimes, persisted grants and dynamic provider permission callbacks
remain absent. This patch also retains the existing owner-user, content-scheme
and ambiguous-path restrictions; it does not claim every URI API edge case or
Android shared-user/profile policy. Successful explicit grants here are actual
entries consumed by the existing access checks, not promises that those separate
backends now exist.

## Pinned primary references

- [ContextImpl: public explicit grant/revoke wrappers](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/app/ContextImpl.java)
- [Context: explicit grant and recipient revocation contracts](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/content/Context.java)
- [ActivityManagerService: caller validation, flag checks and delegation](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/ActivityManagerService.java)
- [UriGrantsManagerService: direct permissions, patterns, grant strength and revocation direction](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/uri/UriGrantsManagerService.java)
- [GrantUri: URI identity and embedded-user normalization](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/uri/GrantUri.java)
- [PackageInfoCommonUtils: projection of provider grant patterns](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/com/android/internal/pm/parsing/PackageInfoCommonUtils.java)
