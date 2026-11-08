# Package compatibility follow-up — 2026-11-08

This follow-up checks the package paths of PR #8 against the repository's pinned
`android-16.0.0_r4` framework. It builds on
[the split-package audit](package-split-contracts-2026-11-08.md); it does not establish
universal compatibility with arbitrary applications.

## Findings and implemented behavior

### Manifest flags were lost before app initialization

`PackageParser2` returns a parsed package before `PackageImpl.hideAsFinal()` fills
the cached public and private `ApplicationInfo` flags. Gesher restored individual
flags, leaving other actual manifest requests absent from the package information
used by app startup. For example, `ActivityThread` uses `FLAG_LARGE_HEAP` to select
the heap growth policy. Missing flags can therefore change runtime behavior, beyond
merely returning incomplete metadata.

`ApkPackage.installed()` now derives public and private flags through the pinned
framework's `AppInfoUtils` helpers. It preserves existing state/install bits,
Gesher's actual-DEX check for `FLAG_HAS_CODE`, system-addon ownership, and the
existing extended-private-flag handling. This publishes the actual manifest policy;
it does not claim that every policy request has an implemented platform backend.

Sources:

- [PackageImpl: derived flags and ApplicationInfo conversion](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/pm/parsing/pkg/PackageImpl.java)
- [AppInfoUtils: public and private flag derivation](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/pm/parsing/AppInfoUtils.java)
- [ActivityThread: FLAG_LARGE_HEAP at application binding](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ActivityThread.java)

### A cached base path hid changes to its companion splits

The source cache records a source file and its `.dm` size/mtime, but not the
companion APK set. Installing a base and later adding or replacing a split beside
the same unchanged base could return the old generation before comparing or
validating the new split. A feature could consequently be absent even after an
apparently completed installation, or a malformed replacement could escape the
normal cluster-validation path.

The fast cache now yields to the actual installation path when either the source
directory or its recorded installed generation has companion APKs. That path
verifies the signer set, compares APK contents, parses the installed cluster and
publishes the new generation. Identical validated inputs still return `unchanged`.

There are two intentionally different empty-split behaviors:

- Opening an unchanged standalone base through Gesher's raw launch/install path
  preserves its installed splits. This existing convenience prevents launching an
  extracted base from discarding required resources.
- A `MODE_FULL_INSTALL` session or complete `.apks`/`.xapk` archive supplies the
  complete APK set. It now removes previously installed optional splits omitted
  from that set, including their activities and providers. Previously the generic
  empty-input shortcut incorrectly inherited them.

Gesher's session service supports full installs; this change does not add
`MODE_INHERIT_EXISTING`. The distinction follows
[PackageInstaller.SessionParams](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/PackageInstaller.java).
The source cache's pre-existing use of base-file size/mtime, rather than a content
digest for every cache hit, remains a separate limitation.

### Missing required splits were not rejected by cluster parsing alone

The pinned installer performs required-split validation outside `PackageParser2`.
When the base requires splits, a full staged set must contain more than the base
and must provide every declared required split type. The existing Gesher cluster
validation covered package/version identity and manifest-named dependencies, but
not this additional contract.

`parseCodeDirectory()` now collects required/provided types from the actual
`ApkLite` records and applies the same rule before publishing a generation. An
incomplete full update fails with `INSTALL_FAILED_MISSING_SPLIT`, removes its
temporary generation, and retains the working package and registry. A complete
required set is installed normally. The raw unchanged-base convenience above
still uses the validated installed set.

The failure carries the framework's typed `PackageParserException` error code.
`PackageInstallerService` maps that code through the pinned
`PackageManager.installStatusToPublicStatus()` helper, so the public commit callback
reports `STATUS_FAILURE_INCOMPATIBLE`, rather than misclassifying it as a storage
failure. The real session fixture checks this callback and retirement of the failed
session.

Sources:

- [PackageInstallerSession: MODE_FULL_INSTALL required-split validation](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageInstallerSession.java)
- [ApkLiteParseUtils: required/provided type parsing and cluster composition](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/parsing/ApkLiteParseUtils.java)
- [ApkLite: isSplitRequired includes required split types](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/parsing/ApkLite.java)

### Ordinary package queries inherited incorrect defaults

`PackageManagerService` now implements these operations using its package and
permission inventory:

| Operation | Implemented result |
| --- | --- |
| `getPackageGids` | An empty supplementary-group array for a known installed package or the platform package; `null` for an unknown package/user. Gesher assigns UIDs but has no per-package supplementary-group assignment inventory. Previously the inherited `null` made `ApplicationPackageManager` throw `NameNotFoundException` for an existing package. |
| `getNamesForUids` | A positional name array retaining duplicate/unknown requests, with the pinned system shared-UID name. A null or empty input retains Android's null return contract. |
| `getPackagesHoldingPermissions` | Actual installed packages granted at least one requested permission by Gesher's existing permission policy. Without `GET_PERMISSIONS`, returned permission names are the matching query subset, preserving order and duplicates; with the flag, the complete manifest inventory and current grant flags are retained. Unknown users and no matches produce an empty result. Packages being removed are excluded. |

These operations do not manufacture packages or grant additional permissions.
The new GID and permission-holder queries honor `MATCH_SYSTEM_ONLY`, using actual
system-addon state; ordinary installed packages are excluded when that flag is
requested.
Their contracts were checked against
[ApplicationPackageManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ApplicationPackageManager.java)
and [ComputerEngine](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java).

### registerDexModule omitted Android's asynchronous completion

Android 16 intentionally disables explicit `registerDexModule` registration and
optimization. Its public documentation and package service specify an asynchronous
callback with `success=false`. Gesher's inherited implementation omitted that
completion, leaving callback-based callers waiting indefinitely.

The service now posts exactly one completion per non-null callback through its
existing worker, using the original module path and Android's exact unsupported
reason. It invokes app callbacks outside the package monitor and without a client
Binder identity; reentrant and disconnected callbacks do not suppress later work.
This implements the pinned platform's callback behavior. It is not a claim that
Gesher gained a secondary-dex registration backend.

Sources:

- [PackageManager: registerDexModule documentation](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/PackageManager.java)
- [PackageManagerService: disabled registration and posted callback](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageManagerService.java)

## Regression evidence

The production replacements and four package fixtures compile with Java 21,
target 17, the actual CI framework classes, Android core module and apksig inputs.
This is a compilation check; it is not a Windows runtime result.

The following tests ran on the host against the real production/framework classes:

- `PackageManifestFlagsTest`: **75 assertions passed**, covering true/false manifest
  settings, preservation of existing state, actual-DEX handling and addon flags.
  Its fixture allocates records without native-backed constructors, then invokes
  production `ApkPackage` projection. Putting the unchanged baseline app JAR first
  on the same classpath fails the first debuggable-flag assertion.
- `PackageServiceQueriesTest`: **22 assertions passed**, covering current/removed
  package identities, unknown users, positional UID names and callback scheduling,
  exact completion identity, reentrancy, null callbacks and disconnected/local
  callback failures, plus system-only identity filtering. It injects a deterministic executor into the production
  service. The unchanged baseline fails its first installed-package GID query.
- A focused scratch diagnostic using production `recordSource`/`recordedSource`
  and real files reproduced the stale-base cache hit before the change and passed
  after the companion guard. It exercises the cache rather than claiming a host
  APK-parser/runtime run.

`tests/apps/splitpackage/build.sh` successfully built **nine actual APK fixtures**
using the pinned resource APK, aapt2, javac and D8. All nine passed `zipalign -c 4`.
Binary manifest inspection verified revision 7, the required-base declarations,
and the matching provided split type. A signed required-base witness also passed
`apksigner verify`, for the real session commit test.

The Windows/ART fixtures are extended for the complete runtime paths:

- `PackageQueryContractTest`: actual parser, production service, generated AIDL
  proxy/Parcel and `ApplicationPackageManager`; query flags, permission subsets and
  duplicates, removed packages, and an asynchronous Binder callback that reenters
  the package service.
- `SplitPackageContractTest`: real same-base-path additions/replacements and
  unchanged retries; invalid replacement rollback; manifest flags; full-session
  and archive removal of optional split components; valid required splits and
  rollback of absent APK/type updates; signed `PackageInstallerService`
  open/write/fsync/commit and failure callback through its actual worker and
  `IntentSender`; actual `LoadedApk` dependency loaders,
  feature classes and compiled resources after successful updates.

These ART fixtures require the Windows runtime and are not reported here as
locally executed. A successful fixture build or host method test does not replace
that runtime gate.

### First focused Windows result and fixture bootstrap correction

The focused Windows 2022 job for published `223d4b3b` reached these real
fixtures and exposed missing setup in their isolated test VMs:
[job 128649121906](https://github.com/gesher-runtime/gesher/actions/runs/42453120565/job/128649121906).
`PackageQueryContractTest` failed while constructing `ContextImpl`: its display
lookup reached the external service manager before the fixture had registered a
display service. `SplitPackageContractTest` performed real APK dex optimization,
then the first installed-package publication failed with
`unable to assign nonce handle: system_server/package_info_cache` because the VM
had not created its writable framework shared-memory region. Neither was a
successful complete package runtime test.

The fixtures now establish those actual prerequisites before the relevant work.
The query fixture registers the production `Services.DisplayManager` Binder and
its existing package Binder in `ServiceManager`'s cache before constructing the
real context. Both fixtures allocate and initialize native
[`ApplicationSharedMemory`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/os/ApplicationSharedMemory.java)
before package-cache use. This matches the relevant production order:
`SystemServer` initializes the writable region before publishing installation
APIs, while application startup supplies service Bindings before context use.
The generated AIDL proxies, public `ApplicationPackageManager`, real installer,
signed sessions and all previous assertions remain exercised.

The query fixture also counts actual package Binder transactions: the first
public query reaches the service, the repeated query uses the framework cache,
and invalidating its native nonce forces a fresh service query. Removal and
re-addition must invalidate a previously cached package result too. No cache is
disabled or switched to a test-mode nonce. The three related package/activity
fixtures compile against the actual Android core and framework with Java 21,
target 17, and convert to DEX; completion on Windows remains the next runtime
gate.

### Second focused Windows result and installed-code path repair

Published `7e62479c` completed both focused Windows jobs:
[Windows 2022](https://github.com/gesher-runtime/gesher/actions/runs/42456227216/job/128657747836)
and [Windows 2025](https://github.com/gesher-runtime/gesher/actions/runs/42456227216/job/128657747923).
The activity-options fixture passed completely on both platforms. The package
query fixture passed context construction and its real native-cache assertions,
then failed in `ApplicationPackageManager.checkPermission`: its isolated registry
had not published `permissionmgr`. The split fixture reached its first actual
staged cluster validation, where strict installed-code containment rejected the
path before publication. These two package fixtures did not pass in that run.

The query fixture now publishes the existing package manager's real
`PermissionPolicy` through its generated `IPermissionManager` Binder and counts
the public permission query's actual Binder transactions. Its service lookup uses
the production `Services.ServiceManagerStandIn`, including the ordinary missing
service result for the unimplemented legacy permission service. Pinned Android's
[`PermissionManager`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/permission/PermissionManager.java)
constructor asks for that optional manager;
[`SystemServiceRegistry`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/SystemServiceRegistry.java)
handles `ServiceNotFoundException` and leaves it absent.
No default permission implementation or framework cache bypass is introduced.

The installer path failure exposed a production inconsistency. The native
`realpath`/`LinuxPath` contract removes the current drive's `/mnt/<drive>` prefix
and normalizes actual disk spelling; the launcher deliberately uses an explicit
drive for its temporary-directory default. `Installer` had constructed new code
generations directly from the uncanonicalized `ANDROID_DATA` value, while
`ApkPackage.parseCodeDirectory` requires the canonical device root and canonical
generation path. The installer now constructs and scans its `app` root from the
canonical configured device root. All newly recorded generation, registry and
source-cache paths consequently use that same namespace. Existing registry
paths retain their strict validation. An existing app root must be a real
directory under `NOFOLLOW_LINKS`; staging uses an atomic directory creation
instead of following an occupied generation. The parser's containment equality,
generation naming and no-follow checks remain intact, with path-only diagnostics
added to rejection errors.

`SplitPackageContractTest` retains its real install, split DEX/resources, signed
session and failed-update checks. It additionally checks the canonical returned
generation, registry entry and reloaded code path, plus rejection without changes
when a file occupies the app root. Its original `Files.createTempDirectory` call
is unchanged, preserving the actual Windows path-alias regression witness.

The maintained host runner `tools/test_installer_code_root.py` compiles the actual
modified `Installer` and `ApkPackage`, without filesystem adapters. **12 checks
passed** using a real root alias, app-root file and symlink, external directory,
generation symlink, and unchanged target sentinel. Substituting only the published
`7e62479c` installer fails with `Installer accepted a file as its APK root`.
This host evidence does not execute APK parsing or prove Windows path conversion.
The two repaired package fixtures and production sources compile against the real
Android core/framework at Java target 17; fixture D8 conversion also passed.
Their complete corrected Windows install/query results remain a subsequent CI
gate, while the linked completed run supplies the preceding failures.

## Remaining scope

The subsequently implemented `queryIntentActivityOptions` is described in the
[activity-options follow-up](activity-options-followup-2026-11-08.md), including
its remaining resolver-choice and visibility limits. This package follow-up
does not implement Android multiuser package state, package visibility policy,
supplementary-group assignment, or every service behind the newly preserved
manifest flags.
