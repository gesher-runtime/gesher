# Persistable URI permissions: Android contract and integration

The grant table is keyed by recipient UID and `(source user, URI, prefix)`.
Gesher currently supports source and target user 0. Its permission state follows
AOSP frameworks/base revision `45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:

- [UriPermission](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/uri/UriPermission.java)
- [UriPermissionOwner](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/uri/UriPermissionOwner.java)
- [UriGrantsManagerService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/uri/UriGrantsManagerService.java)
- [ActivityManagerService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/am/ActivityManagerService.java)
- [ContentProviderHelper](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/am/ContentProviderHelper.java)
- [ContentProvider](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/content/ContentProvider.java)

## Permission state

| State | Lifetime and effect |
| --- | --- |
| Read/write owner memberships | Each Activity/service owner contributes independently to the canonical record. Retiring one owner removes only that membership. |
| Global read/write modes | Ownerless grants survive individual owner retirement. |
| Persistable read/write offer | Records which modes the recipient can take. It is excluded from the active mode bitmask. |
| Persisted read/write modes | Survive owner retirement, force stop, valid package update and runtime restart. |

Active mode flags are `owned | global | persisted`. Android's separate
`getStrength()` calculation checks persistable offers before global and owned
modes; `checkUriPermissionLocked()` uses that calculation. Consequently, a partial
release can leave a retained offer usable while another active mode keeps the
canonical record alive. Releasing its last active mode removes the record.
The implementation and independent oracle preserve both calculations rather than
substituting a new permission rule.

Exact and prefix grants remain separate identities. Take/release examines both
identities at the **requested URI**, not an ancestor prefix. A take refreshes the
timestamp as in AOSP; only a persisted-mode change or quota pruning schedules a
write. Zero requested modes are accepted, unsupported mode bits are rejected,
and an existing unpersisted record may be released without an error. The
per-recipient-UID persisted limit is 512 records.

Shared-UID peers use one grant table and can take/release their UID's modes.
The first canonical recipient package remains the incoming-list filter, matching
AOSP. UID admission and signing compatibility use the real package UID registry;
the URI fixtures seed admitted metadata and do not replace that registry's signed
APK tests.

Public lists use framework `android.content.UriPermission` objects. Privileged
administration returns framework `android.app.GrantedUriPermission`, filters
recipients, and accepts the null filter for all recipients of user 0. Explicit
`toPackage` persistence operations require `FORCE_PERSISTABLE_URI_PERMISSIONS`,
including when the package matches the caller. Incoming/outgoing enumeration
requires a package belonging to the calling UID.

Advanced `checkUriPermission` queries retain their persistable strength and prefix
identity. The framework `DocumentsProvider` itself queries `READ | PERSISTABLE`
when preserving an offer across canonicalization or rename.

Retained permission queries and `takePersistableUriPermission` consult the
canonical permission and its modes, independently of whether the provider is
currently enabled. This follows `checkUriPermissionLocked` and
`takePersistableUriPermission` in the pinned AOSP service. Disabling a package or
provider component does not erase an existing offer, persisted mode or live
owner membership. Installation epochs and owner retirement still constrain the
retained record. Provider acquisition, access and new grant publication retain
their separate current-provider and admission checks; a successful permission
query does not make a disabled provider available.

Provider acquisition recognizes ownership by authenticated UID, including a
different package admitted into the provider's shared UID. AOSP's
`ContentProviderHelper.checkContentProviderPermission` checks the provider UID
through component permission checks; `ContentProvider` also recognizes the
owner's app identity before exported/read/write policy. Gesher validates the
current admitted package identities and provider metadata before matching UIDs.
Missing, pending, negative or retired identities do not establish ownership.
An unrelated UID still needs the provider's exported permissions or a real URI
grant. Acquiring a provider does not grant unrestricted access to its contents.

## Installation identity and durability

Prepared grants retain the actual source registration/installed-package object
and the actual target object. Publication validates the complete prepared batch
before adding any permission. Replaced runtime objects retire transient rights;
persisted rights instead use the stable package installation-owner epoch already
maintained by `PackageEnabledStore`. Valid updates preserve that epoch. Uninstall
rotates it, so a previously committed URI snapshot cannot restore an earlier
installation's capabilities after a crash and reinstall.

Restoration resolves the URI's actual provider authority and verifies its package
owner and current installation epoch, then resolves the recipient's current UID.
It restores persisted modes and their offers, without inventing a global grant or
owner membership. The epoch attributes are an internal persistence detail; they
do not change the client API. Legacy records without epoch attributes still
undergo actual authority and installed-package validation.

Force stop clears transient rights. Permanent package-removal callbacks clear
both transient and persisted rights. A temporarily disabled provider does not
erase its retained permission records or prevent an eligible retained mode from
being taken. Explicit revocation and release continue to change that state.

Writes use Android `AtomicFile`, an immutable snapshot and a single queued writer.
Changes coalesce with the AOSP ten-second delay. Parsing and writing occur outside
the package and grant monitors. Ordinary lookups use package-before-grant lock
ordering, and owner callbacks acquire only grant state. `flush()` waits outside
those monitors and is available for orderly shutdown and deterministic fixtures.
An older in-flight snapshot cannot cancel a later scheduled mutation.

## Actual server-hosted providers

`ContentProviders.RegisteredProvider` contains the actual provider metadata and
transport in an immutable registry snapshot. No APK is fabricated for a hosted
provider. A trusted picker obtains prepared permissions through
`prepareFromProvider(registration, targetPackage, uri, modes)` and publishes the
complete selection with `publish(activityOwner, preparedGrants)`. Preparation
verifies the exact live registration, authority, recipient and grantable paths.
Publication repeats generation validation for the whole batch. The Activity's
ordinary retirement drops only its transient memberships; a mode actually taken
by the recipient remains persisted. The public Binder system/root restriction on
directly delegating arbitrary provider URIs remains in place.

The same scoped provider context uses `revokeFromProvider(registration, uri,
modes)` (or its target-package overload). It verifies the exact live registration
and authority before ordinary AOSP revocation. The untargeted overload removes
matching owners and persisted permissions for all recipients, including the `~0`
mask used by `DocumentsProvider.revokeDocumentPermission` after rename/delete.

This change supplies the registry and grant integration surface. A hosted
DocumentsProvider, document storage and native picker are separate integration
work; the empty registry alone advertises none of those capabilities.

## Validation and limits

- `UriPermissionModesOracleTest`: 27,326 assertions against the unmodified pinned
  AOSP `UriPermission` and `UriPermissionOwner`; source blob IDs are recorded in
  `tests/uri/aosp/README.md`.
- `PersistableUriGrantContractTest`: 99 production-service checks, including
  exact/prefix take/release, public/admin result types, timestamps, quota,
  shared UID, owner retirement, actual PM metadata publication, AtomicFile
  incomplete-write recovery, restart, installation epochs, removal callbacks,
  hosted-provider batch publication and blocked-file-IO concurrency. Disabled
  package/component cases exercise first and repeated take, exact and prefix
  queries, unoffered-mode rejection, transient owner retirement, source/recipient
  installation epochs, explicit revocation and the separate access check.
- `UriGrantContractTest`: 154 checks. Existing validation expectations now match
  Android's no-op behavior for absent URI/access modes, and the fixture represents
  admitted shared UIDs rather than an accidental UID collision. The provider
  admission cases publish through the production registry and acquire/release
  through AMS. They cover shared-UID private access, unrelated-UID denial, real
  URI-grant access/revocation, connection cleanup, invalid metadata and retiring
  identities.
- `IntentUriGrantContractTest`: 28 checks through production preparation,
  publication and started-service owner retirement.
- `TaskStackContractTest --case tasks`: 59 host checks, including reordered,
  abandoned, force-stopped and retained-after-process-death Activity URI owners.
- All 280 production Java sources type-check against the cached Android 16
  framework/core API input. The complete service fixture suite builds to DEX.
  Both signed package-event target versions build with a real declared provider;
  `PackageUninstallTest` now grants/persists its URI through production code
  instead of injecting an obsolete private `Grant` object.

The host fixture uses explicit Binder caller-identity/native-initialization
boundaries and constructed parsed metadata. Native framework resources are
unavailable there; tests requiring the full framework permission catalog remain
Windows gates. No host result claims remote Binder transport, ART, a window,
an actual document selection, or a signed APK uninstall/reinstall execution.
Windows track 94 requires the exact URI/persistence check totals; the removal
fixture is also registered by the existing Windows compatibility runner.
The existing Windows `ProviderSecurityTest` reads APK UIDs and provider metadata
after package admission, issues grants as the authenticated provider owner, and
expects Android's `SecurityException` for a missing persistable offer. Its
publication, reference, lock-order and retirement assertions remain in place.

A fresh `app/build.sh` attempt regenerated the Binder stubs but stopped at its
required compatibility-table generation gate: the cached framework API and
partial source checkout contain no usable `@ChangeId` fields. That gate was not
bypassed. The full production-source type-check uses the previously generated
Stub/CompatChangeTable dependency archive. A complete current-commit framework
build and Windows/ART execution are still required CI evidence before merge.
