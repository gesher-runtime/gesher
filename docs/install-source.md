# Package installation attribution

Gesher records the authenticated package that initiated an installation, its verified
signing snapshot, the declared package-source category, and the current installer of
record. `PackageManager.setInstallerPackageName` changes only the current installer.
It does not rewrite the initiating package, its historical certificates, or the source
category. A category supplied by a session without an application identity is retained
without manufacturing an installer.

The setter follows Android 16's visibility and signing checks. The caller must be an
authenticated attached process; both the target and a non-null new installer must be
visible. The caller's verified **current** signer must match both the new installer and
an existing installer. A rotation capability alone is not certificate equality for this
API. If there is no current installer, `INSTALL_PACKAGES` is required: applications
targeting R or later receive `SecurityException`, while older applications are denied
silently. The decision uses the authenticated application's target SDK; this runtime
does not yet admit Android shared-user groups. A numeric UID collision does not combine
identities.

Session APIs also authenticate the process's attached package code. A live process
with a pending exit keeps its own session authority until death, but cannot create
or open sessions using a later installation's identity or signing snapshot.
Rebuilding missing compiled output for unchanged APK bytes retains that installed
identity. An explicit same-code session can still update attribution durably; a
failed registry write leaves both the current identity and attribution intact.

The target can see both its current installer and its still-installed initiating
package. Other callers filter those names independently. After the initiating package
is removed, only the authenticated target application can retrieve its historical
identity and signing information. Merely sharing a numeric UID in a legacy profile
does not grant that access. Each relationship carries
an installation-owner UUID, so reinstalling the same package name cannot revive a
retired relationship.

Updates are serialized in Installer-to-PackageManager lock order. Process authentication
runs before those locks, and authorization is revalidated with the current package
snapshot. The synchronized registry replacement is flushed before in-memory publication;
a failed write leaves the previous attribution intact. Ordinary installed-code rows keep
reading older registry formats. Bundled and command-line packages use metadata comment
rows tied to their real installation owner, without fabricated code entries or paths.

## Contract references

These rules are based on the `android-16.0.0_r4` implementations of:

- [PackageManagerService.setInstallerPackageName](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageManagerService.java)
- [ComputerEngine.getInstallSourceInfo](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java)
- [InstallSource.setInstallerPackage and removeInstallerPackage](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/InstallSource.java)
- [AppsFilterUtils.canQueryAsInstaller](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/AppsFilterUtils.java)
- [PackageManagerServiceUtils.compareSignatures](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageManagerServiceUtils.java)

## Validation

`tools/test_install_source.py` runs the actual Java package manager, full APK verifier,
installer registry and public framework wrapper on a host JVM. It
uses the repository's signed v2/rotated APK fixtures, constructed manifests and a
controlled signature-permission catalog; native Binder identity and allocation are
explicit host boundaries. It checks reassignment, visibility, authenticated caller
identity, SDK compatibility, retired-owner handling, coherent reads, failed-write
rollback, metadata restart, and self queries that avoid traversing the package inventory.

Pass `--java-home`, `--framework-classes`, `--core-classes`, `--app-classes`, and `--apksig`
with the corresponding build paths. An optional `--output` retains its artifacts in a
new directory. `tools/test_package_visibility.py` also covers both independent
installer/initiator visibility edges. The latter's Android fixture is included in
Windows track 94. Host results do not qualify Windows/ART execution, native Binder
transport, or the real framework-resource permission catalog.
