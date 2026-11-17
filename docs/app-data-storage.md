# Application credential and device storage

This implements the separate disk areas of issue #56 F50 for Gesher's existing
single-user, always-unlocked desktop profile. The Android contract explicitly
requires separate storage locations even on devices that do not encrypt the two
areas with different keys. This change does not add encryption, an Android
pre-unlock state, or a new Windows process sandbox.

## Paths and public behavior

| Area | Installed application path |
| --- | --- |
| Credential-protected (CE) | `$ANDROID_DATA/data/<package>` |
| Device-protected (DE) | `$ANDROID_DATA/user_de/0/<package>` |
| Private layout/recovery metadata | `$ANDROID_DATA/system/app-data-layout/<package>` |

`ApplicationInfo.credentialProtectedDataDir` and `deviceProtectedDataDir` identify
different directories. `dataDir` selects CE normally. A verified system package
can select DE by declaring `android:defaultToDeviceProtectedStorage="true"`.
As in AOSP's package scan policy, ordinary APKs cannot enable that default:
their manifest request is ignored and the corresponding published private flag
is cleared. They can still explicitly create a device-protected context. AOSP
`LoadedApk` and `ContextImpl` therefore select the right area without overriding
the public storage APIs. Files, preferences, databases, cache and code cache
remain separate, including identical names in the two contexts.

An explicit in-process `--data-dir /path/app` retains `/path/app` as CE. Its DE
area is `/path/app.device-protected`; its layout metadata is
`/path/app.gesher-storage`. Sibling paths avoid nesting one area in the other.

Both areas are prepared before an app starts. Package metadata parsing alone
does not prepare, migrate or rename them. Storage statistics include both areas
once, including each area's cache/code-cache directories. The existing
`UserManager.isUserUnlocked(0)` and mount-service CE-unlocked result remain true;
the desktop profile does not pretend that locking Windows creates an Android
first-unlock transition.

## Upgrading the old aliased layout

Ordinary legacy applications keep their files at the existing CE path. No
unrequested bulk copy to DE occurs. Apps can move selected preferences or
databases through the existing Android `moveSharedPreferencesFrom` and
`moveDatabaseFrom` APIs in either direction.

For a **previously installed system application whose committed manifest already
declares an effective DE default**, startup performs a one-time rename of the old shared
area into DE, preserving the data its default context used. It then creates an
empty, separate CE area. The old runtime did not record which explicit context
wrote a file; that lost provenance cannot be reconstructed. The migration
preserves the old default area, without duplicating all files into both areas.

A canonical per-app JVM guard is acquired **before opening the lock file**;
then a file lock excludes other processes. Both waits share one ten-second
budget, and interruption is preserved. Nonblocking file-lock acquisition uses
the supported POSIX operation instead of the unfinished blocking-lock contract.
The local guard stays held until the file channel closes, and its reference-counted
registry discards entries when all owners/waiters finish. Root aliases resolve to
the same guard. A versioned, file-synced
journal records the impending move before the rename and marks the layout ready
before creating empty CE. Recovery handles crashes immediately before or after
the rename. This is process-crash recovery; power-loss durability of directory
entries has not been established. A completed marker prevents migration from recurring when an app
later changes its manifest default. A conflicting DE destination, malformed
journal, missing recovery data, symlink or blocked directory produces an error
and leaves the original data available; directories are never silently merged
or replaced.

Only the already installed manifest may authorize that legacy migration.
Registry parsing does not finalize storage before the system-package scan has
established trusted status. Startup prepares the committed record after that
classification, even when a different bundled version is selected for launch.
Only then does it prepare the selected APK. An ordinary application's raw
DE-default request therefore cannot move its old CE data; a fallback or incoming
manifest cannot reclassify the old application's default either.
Installer staging and incoming APK flags cannot move old data. If an unusual
update path encounters an unprepared old layout, it reports the problem before
publication; normal device startup first prepares the installed version. A bare
APK or custom `--data-dir` launch cannot establish the previous manifest's
identity, so ambiguous legacy DE-default data is also preserved with an explicit
error instead of guessed migration. Its old data remains at the documented CE
path for deliberate recovery.

## Removal and recovery

Uninstall quarantines CE, DE and the layout metadata before removing the package
from the persistent registry. Failed or interrupted uninstalls restore all
three. Committed removals reap the quarantined generation, so reinstall cannot
inherit either old area or an old layout marker.

The original quarantine names `item-0` through `item-4` retain their exact old
meaning. Added areas use fixed names `device-protected-data` and `storage-layout`.
This preserves recovery of old journals and avoids collisions with other PRs
that add named quarantine slots.

## Verification and remaining gates

`tools/test_app_data_storage.py` compiles the exact production `AppDataStorage`
and `PackageRemovalStorage` sources against only two host boundary adapters
(device-root lookup and the retry sleep). It exercises real files, journal
replacement, directory renames and locks. It covers independent filenames,
legacy/default-change behavior, forbidden migration from incoming metadata,
pre/post-rename recovery, collisions, invalid versions, blocked paths, symlinks,
four independent preparer processes, uninstall rollback/reinstall, and recovery
of the original positional journal format. The standalone CI workflow runs this
host contract suite.

Independent QA found and reproduced a subtle ownership bug in the initial
implementation: under traditional POSIX process-owned locks, closing a canceled
waiter's second descriptor can release another thread's still-live file lock.
Windows' current record-lock backend has that same close-any-descriptor behavior.
The local arbitration above fixes that failure without changing native locking.
The regression acquires the actual production lock scope, cancels same-JVM
contenders using both normal and aliased root paths, and asks an independent
process to prove it remains excluded until the owner finishes. Recursive
preparation is rejected before a second descriptor opens; cancellation, normal
completion and error paths release the local registry references. These are host
filesystem/process tests, not a claim that the Windows ART run has completed.

`tools/test_app_storage_public.py` runs the same fixture against either a baseline
or changed app-classes jar. It uses actual `ApkPackage` metadata projection,
`LoadedApk` construction and `ContextImpl` path/file methods, with controlled host
JNI boundaries and the resource-dependent Context constructor bypassed. The
baseline fails on the explicit public CE/DE-alias assertion; the changed jar
preserves different contents for an identical filename with every combination
of ordinary/system status and the requested manifest default,
including newly constructed contexts. This is stronger than comparing path
strings in a replacement model, but does not replace the target ART fixture.

The Windows ART fixtures cover the production metadata projection, real
`ContextImpl` file/preference/database operations and moves, actual service
statistics, and installer/uninstall cleanup. They also run actual layout
preparation, journal replacement, legacy migration and restart through ART's
filesystem and locking backend. `AppStorageContextTest` is required
by Windows track 94; the existing `SettingsIntentsTest` and
`PackageUninstallTest` also assert DE behavior. Host filesystem tests and Java/D8
compilation do not establish that Windows ART or physical suspend/unlock tests
passed. Those target-platform runs and an independent Android comparison remain
required before closing F50 in the roadmap.

The same host runner also executes `AppStoragePolicyHost` against production
`ApkPackage` and `SystemServer` preparation. It verifies signed/unsigned status
through the real APK verifier, preserves ordinary CE data even with a manifest
DE-default request, and covers classification before preparation, committed
migration authority, fallback/default changes, restart, interrupted migration
and rejection without publication when the committed layout is ambiguous.

Windows CI also exposed two integration expectations missed by the initial host
suite. Startup retains the documented `system_server: <package> parsed by
PackageParser2: ...` diagnostic, emitted only after the whole final selection is
prepared. The host runner observes the real preparation boundary and checks the
exact summary format/order, completed layouts at log time, and absence of success
summaries after a later failure or exclusion. The installed split-APK fixture now
explicitly requires its ordinary manifest's default-DE request to be cleared,
its default path to remain CE, and its explicit DE path to remain distinct. Its
other manifest flags, split code/resources and lifecycle checks remain in place;
the same ordinary metadata-policy assertions also run on the host, alongside the
existing verified-system positive and unsigned-promotion rejection cases.

## Contracts consulted

- [Android Context.createDeviceProtectedStorageContext](https://developer.android.com/reference/android/content/Context#createDeviceProtectedStorageContext())
- [Android Direct Boot guidance](https://developer.android.com/privacy-and-security/direct-boot)
- [AOSP Android 16 ContextImpl storage selection and migration](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ContextImpl.java)
- [AOSP Android 16 LoadedApk application storage paths](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/LoadedApk.java)
- [AOSP Android 16 system-package scan policy](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/ScanPackageUtils.java)
