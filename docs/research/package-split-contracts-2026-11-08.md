# Installed split APK contracts (2026-11-08)

The compatibility review found that installing a split APK set copied its files
but parsed only `base.apk`. `ApkPackage.installed` then synthesized split names
from sorted filenames. Feature component declarations were absent, and an
isolated feature did not receive its dependency graph or class-loader policy.
An app could therefore fail to find a declared activity/provider, fail to load
a feature class, or select a resource path with the wrong split index.

## Implemented behavior

`ApkPackage.parseCodeDirectory` parses an installer-owned generation under
`$ANDROID_DATA/app/<package>-<n>` with AOSP `PackageParser2`'s cluster path.
It requires a real `base.apk`, rejects links and paths outside that code tree,
and checks the package name and full long version of each split. AOSP checks
duplicate split names and the isolated feature dependency graph, merges the
feature manifests, and supplies split names, paths, class-loader names and
dependency indices together. `ApplicationInfo` also carries the parsed isolated
split loading flag; this flag normally comes from `PackageImpl.hideAsFinal`,
which Gesher's parser path does not call.

Installation parses the staged cluster and verifies its signatures before
stopping a running version or publishing a registry entry. A malformed cluster
removes its staging generation and preserves the current install. Registry
reload, unchanged-source handling and dexopt library-context resolution use
the same complete manifest graph. A byte-identical lone base keeps the existing
installed splits, matching the installer's existing behavior.

The regular launcher installs through `Installer` and resolves components
through the system server's package manager. `Main --system-server` obtains
`ApplicationInfo` from that manager, and the framework `LoadedApk` consumes the
manifest split graph. Installed Start menu shortcuts and URL handlers now parse
the same code generation; icon rendering includes the published resource splits.

`ApkPackage.parse(file)` still means a single APK. It never discovers arbitrary
neighboring APKs. The direct standalone `com.gesher.app.Main` driver and explicit
extra-APK `SystemServer` test bootstrap use this single-file path. Run installed
feature sets through the regular launcher/system-server path. Signature-only
queries, external icon inspection and addon parsing also keep their single-file
input semantics. The installer's existing `split_*.apk`/`config.*.apk` companion
import remains an explicit installation step; cluster parsing happens after
those files are copied to the private generation.

This repair does not implement on-demand feature delivery or change dexopt's
existing base-only AOT compilation. Feature DEX is loaded by ART through the
framework's split class loaders. General multiuser behavior and unrelated
package-manager defaults require separate work.

## Regression and validation

`tests/apps/splitpackage/build.sh` builds a real base APK and two isolated feature
APKs with independent DEX and resources. The entry feature depends on the core
feature and selects `DelegateLastClassLoader`; its components run in a secondary
app process. Physical filenames deliberately sort opposite to the manifest names.
The fixture includes a foreign-package split and a split with an inconsistent
`versionCodeMajor` for rejection tests.

`SplitPackageContractTest` exercises the production installer and registry,
package/component metadata, launcher discovery, actual `LoadedApk` DEX loading,
inherited resource paths, resource lookup, provider process ownership, repeated
installation and reboot-style registry reload. Negative cases cover an unknown
split, access through an unrelated package name, external directory parsing,
missing dependencies, duplicate names, foreign packages and long-version
mismatches. Failed updates must preserve the package object and registry bytes
and leave no extra code generation. The Windows framework-fixes track runs this
test with the built fixture.

Local validation in the review environment built all fixture APKs with aapt2,
javac and D8, inspected their binary manifests, and compiled the changed
production Java and regression against the project's framework classes.
Windows ART execution is a separate CI gate; compilation alone is not evidence
that the runtime assertions have passed.

## Primary references

The repository pins Android `android-16.0.0_r4`:

- [ParsingPackageUtils: monolithic versus cluster parsing, split application parsing](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/pm/pkg/parsing/ParsingPackageUtils.java)
- [ApkLiteParseUtils: cluster identity, version and duplicate-name checks](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/parsing/ApkLiteParseUtils.java)
- [LoadedApk: isolated split dependency, resource and class loaders](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/LoadedApk.java)
- [PackageImpl: parsed split metadata and derived application flags](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/pm/parsing/pkg/PackageImpl.java)
