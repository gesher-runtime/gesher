# UserDictionary provider (F15)

This supplies the **UserDictionary portion** of issue #56 F15. Contacts, Calendar
and Downloads providers remain separate work. The authority is
`user_dictionary`; word rows use `content://user_dictionary/words` and
`content://user_dictionary/words/<id>`.

## Android contract and source

The implementation ports the Apache-2.0
[Android 16 r4 UserDictionaryProvider](https://android.googlesource.com/platform/packages/providers/UserDictionaryProvider/+/refs/tags/android-16.0.0_r4/src/com/android/providers/userdictionary/UserDictionaryProvider.java).
Its original copyright and license notice are retained. The decoded source's
SHA-256 is `76184729ffb6e230fb909c2d0118cf2301654d632a121840592e0f3986524bf2`.

The matching
[provider manifest](https://android.googlesource.com/platform/packages/providers/UserDictionaryProvider/+/refs/tags/android-16.0.0_r4/AndroidManifest.xml)
exports that authority without read/write permissions. The
[public UserDictionary API](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/provider/UserDictionary.java)
documents access through enabled input methods and spell checkers since API 23.
The legacy `READ_USER_DICTIONARY` and `WRITE_USER_DICTIONARY` permissions are
marked removed in the matching
[framework manifest](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/res/AndroidManifest.xml).
Requesting those removed permissions does not authorize this provider.

| Surface | Preserved behavior |
| --- | --- |
| Schema | SQLite version 2, table `words`, columns `_id`, `word`, `frequency`, `locale`, `appid`, `shortcut` |
| Query | Real SQLiteQueryBuilder projection, selection, bind arguments and sort; default `frequency DESC`; collection and item URIs |
| Insert | Requires the `word` key; permits explicit SQL null; copies input; defaults frequency to 1 and locale/shortcut to null; sets `appid` to 0 |
| Update/delete | Item ID is combined with the caller's selection; normal SQLite affected-row counts and errors |
| Public helper | Framework `Words.addWord` skips empty words, clamps frequency to 0–255, and uses `Locale.toString()`; direct inserts retain their supplied frequency |
| Bulk/batch | Framework default behavior, including operation back references and earlier side effects surviving a later batch failure |
| Cancellation | The pinned provider implements the legacy query method, so the inherited query adapter does not cancel its SQLite operation |
| Notifications | Query cursors watch the requested URI; inserts notify the new item URI; updates/deletes notify their supplied URI even when no row matched |
| Durability | Actual SQLite database and schema upgrade; version 1 retains its rows and receives the nullable shortcut column |

The data contract deliberately preserves Android's details. In particular,
inherited `bulkInsert` counts attempted inserts even if restricted insert calls
return null, and an already canceled signal does not turn the legacy provider
into a different cancellation contract. SQLite rejects surplus bind arguments
with `IllegalArgumentException` on the API 36 reference runtime.

## Access and runtime integration

Root and system callers retain the source provider's privileged access. An
ordinary application must be an authenticated, currently installed caller whose
package and UID match an enabled input method or spell checker. A claimed
package name alone is insufficient. Process death, package removal and pending
removal revoke this access. The production code asks the real framework input
method and text-services managers for their current enabled services; it does
not install a substitute enabled service or grant a removed permission.

Gesher can host an application and its services in one process, or run them
under the same guest UID in legacy mode. Consequently the original provider's
blanket provider-UID exemption cannot authorize every co-hosted application.
The port checks the authenticated process package before treating a local call
as system work. An attached application still needs the enabled-service match.
Framework attribution checks remain active in addition to that matching.

An ordinary restricted query returns an empty cursor with validated column
names. Insert returns null; update and delete return zero. The ordering of these
checks and URI/value validation matches the source. MIME queries remain public.

The provider lives in the system service process and is acquired through the
ordinary ContentProviders/ContentProvider transport. Its database resides at
`$ANDROID_DATA/system/user-dictionary/databases/user_dict.db`.
This system-owned directory cannot collide with a previously installed APK's
`data/<package>` directory or its ordinary uninstall cleanup.
The holder uses the actual prepared framework context and resolver.
It publishes the provider transport only after initialization completes. If
`attachInfo` fails after installing a context, later acquisitions keep reporting
the original initialization cause; that partially initialized instance is not
reused. Any database helper allocated before the failure is closed. Recreating
the runtime-owned provider after a system-server restart allows a fresh attempt.
Before an application is bound, that context initializes the SDK feature cache
from the available package manager's real feature catalog. Existing application
feature caches are retained. Missing package service or failed Binder calls
remain startup errors; no substitute feature list is installed.
Notification and enabled-service calls clear and restore the incoming Binder
identity to preserve the service identity that separate AOSP provider-process
calls have.
Mutations also invoke the framework BackupManager's `dataChanged` request; this
change does not implement a new backup transport.

The installed provider owner is `com.android.providers.userdictionary`.
Package/application/provider lookup, enumeration, UID reverse lookup and
package visibility describe the built-in code. The reported class is the
actual port class, `com.gesher.server.UserDictionaryProvider`, and its code path
is the actual runtime `framework/gesher-app.jar`. The package metadata reports the
manifest's `android.uid.shared` identity and logical `android.process.acore` name.
Gesher reserves a distinct app-range UID for the built-in package in its normal
persistent allocator. The provider runs in the existing service process, and its
logical UID confers no system permissions or APK signing identity. Ordinary APKs
cannot join the built-in identity. User 0 is the supported owner. Unavailable
users receive no dictionary package/provider metadata.
Direct-boot component filters reflect the source manifest's unaware provider.

Provider enumeration also follows
[Android 16 r4 ComputerEngine.queryContentProviders](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java):
it authorizes the derived target user before returning even an unavailable-user
empty result, then filters each provider by the caller's package visibility.
The existing process-authenticated permission policy accepts the actual root or
system UID or a genuine `INTERACT_ACROSS_USERS_FULL`/`INTERACT_ACROSS_USERS`
grant. Knowing a process name or UID does not bypass these checks. The shared
PackageVisibility snapshot retains ordinary own-package and manifest query
relationships, including declarations for the dictionary package or its exported
`user_dictionary` authority. Android's package visibility applies to dictionary
metadata lookup and enumeration. Provider transport acquisition still uses the
ContentProviders authorization and the provider's own access checks.

In Main's standalone launch, GesherAppDriver enrolls the installed process
with ATM before binding its ActivityThread application. The shared
PermissionPolicy cross-checks the locally bound package against any
authenticated ATM attachment before considering a service-local exception. It requires
the actual local UID and current installed owner, rejects pending removal or
contradictory attachment, and evaluates the existing package grant policy.
Missing, removed, unsigned or mismatched identities cannot inherit service
privilege. A genuine local service with no bound application keeps its existing
authority. ActivityManager's process permission checks use the same helper.

Incoming APKs cannot replace this package or declare its authority, including a
disabled provider or a semicolon-separated authority alias. Admission runs
before unchanged-install returns, final split-cluster publication and installed
registry loading. Conflicting package/authority installation returns Android's
`INSTALL_FAILED_UPDATE_INCOMPATIBLE`/`INSTALL_FAILED_CONFLICTING_PROVIDER`
errors. Registry removal cannot retire the built-in provider. Word rows belong
to the user's dictionary; uninstalling an input method revokes its access and
does not erase the user's dictionary.

Authority admission reads the authoritative parsed provider declarations,
including disabled and direct-boot declarations and each semicolon-separated
alias. It does not synthesize installed `PackageInfo` merely to check ownership.
Ordinary APK construction still resolves and validates the actual shared-library
graph; a missing required library remains an installation error.

## Validation

The Linux host policy gate recompiles the changed production classes and runs
them in both compatibility modes:

```sh
python3 tools/test_user_dictionary_policy.py \
  --java-home "$JAVA_HOME" \
  --framework-classes out/framework/classes \
  --core-classes out/coremod \
  --app-classes out/app/gesher-app-classes.jar \
  --apksig-jar "$APKSIG_JAR"
```

This checks restriction/error ordering, genuine process/package/UID matching,
enabled spell-checker access, forgery and lifecycle rejection, shared-UID/local
application behavior, metadata, and built-in authority ownership. Native
identity/initialization, parsed manifest construction, process enrollment and
enabled-service replies are explicitly controlled fixture boundaries. It does
not emulate SQLite or claim cross-process Binder coverage.

The runner also executes `ProviderEnumerationPolicyHost` in both modes. Its
regressions cover hidden private/exported providers, explicit process and
metadata queries, legitimate visibility relationships, both cross-user
directions, unavailable-user ordering, root/system access and actual
signature-permission decisions. Binary framework permission definitions,
parsed platform/application records and process enrollment are declared host
boundaries; signature checks use the existing signed/unsigned APK fixtures and
the production APK verifier and permission policy. No grant decision is stubbed.

For independent negative controls, `--source-ref <old-commit>` compiles that
revision's PMS and PermissionPolicy against the current tests. Select `--case visibility`,
`--case cross-user-outbound` or `--case cross-user-inbound` to reproduce each
earlier enumeration failure independently, or `--case local-permissions` for
the standalone application permission path. Local controls cover ordinary
normal-permission access, real signed cross-user grants, unsigned requests,
unrelated packages claiming another UID, stale/removing owners, conflicting
identities and service calls with no bound app. Real ActivityThread binding fields test both matching
and absent ATM attribution. Permission results are not replaced.
The default `--case all` additionally checks permission-bearing remote callers
and the original dictionary policy.

`UserDictionaryContractTest` runs the actual Android SQLite/OpenHelper,
CursorWindow and framework helper/operation code. It checks CRUD, Unicode/null
values, projection/selection/sort, notifications, batch partial effects,
cancellation behavior, close/reopen durability and the version-1 upgrade. Its
local observer-registration and backup services are declared fixture boundaries;
it never enqueues backup work on the reference device.

The same fixture can compare both provider classes on an owned API 36 device:

```sh
python3 tools/build_user_dictionary_reference.py \
  --java-home "$JAVA_HOME" \
  --framework-classes out/framework/classes \
  --core-classes out/coremod \
  --app-classes out/app/gesher-app-classes.jar \
  --r8-jar "$R8_JAR" --output out/user-dictionary-reference.jar

adb push out/user-dictionary-reference.jar /data/local/tmp/user-dictionary-reference.jar
adb shell chmod 0444 /data/local/tmp/user-dictionary-reference.jar
adb shell env CLASSPATH=/data/local/tmp/user-dictionary-reference.jar \
  app_process /system/bin gesher.services.UserDictionaryContractTest \
  com.gesher.server.UserDictionaryProvider
adb shell env CLASSPATH=/data/local/tmp/user-dictionary-reference.jar \
  app_process /system/bin gesher.services.UserDictionaryContractTest \
  com.android.providers.userdictionary.UserDictionaryProvider
```

The builder verifies the unmodified reference source's SHA-256. An existing
download may be supplied with `--reference-source`. Each invocation creates and
removes its own temporary database directory.

Windows track 98 runs that data fixture and `UserDictionaryBootstrapTest` on
Gesher ART. Its raw-ART fixture runner installs the actual runtime's
`$ANDROID_ROOT/framework/framework-res.apk` before the data fixture opens SQLite,
as Android's `app_process` does for the unchanged reference corpus. The bootstrap
fixture checks both initialization and retention of the real package-manager
feature cache, acquires the actual provider holder from the actual service map,
creates and queries a real SQLite row and receives its notification through the
actual ContentService. It also checks repeated holder identity and installed
metadata, and deletes only its own unique test row. This is an in-process
transport gate; it does not claim a remote application or a complete
keyboard/spell-checker user flow.
