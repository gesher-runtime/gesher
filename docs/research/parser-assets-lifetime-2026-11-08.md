# Deterministic parser APK asset lifetime

## Observed failure

The Windows 2025 framework-contract job for `f0d0479f65f1e33dc7f269f2f7fb670619f6fdce`
failed in the existing `SplitPackageContractTest.cachedSourceUpdates` at its ordinary
`Files.write(base.toPath(), inputs.get("base-required"))`. Java reported a
`FileSystemException: Invalid argument` from `UnixFileSystemProvider.newByteChannel`.
The file was the original mutable source `cached-source-device/source/base.apk`, after
several successful installation/update cycles. The corresponding Windows 2022 job passed.
The original fixture, including this write, remains unchanged in this repair.

Evidence: [Windows 2025 job 128670338549](https://github.com/gesher-runtime/gesher/actions/runs/42460310568/job/128670338549)
and [Windows 2022 job 128670338505](https://github.com/gesher-runtime/gesher/actions/runs/42460310568/job/128670338505).

The native audit found that an ordinary full-range unmap closes both the Windows view
and section handle. Windows rejects truncation while a mapping remains open; Gesher's
open/truncate route uses `CREATE_ALWAYS`, and its generic Win32 error conversion can
report `EINVAL` for `ERROR_USER_MAPPED_FILE`. The failing log does not include the raw
Win32 error number, so that final error-code correspondence is an inference. The Java
ownership defect itself is reproduced deterministically below; it does not depend on
that inference or on a particular garbage-collection schedule.

## Pinned ownership contract and correction

All source comparisons use `android-16.0.0_r4`:

- [DefaultSplitAssetLoader](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/com/android/internal/pm/split/DefaultSplitAssetLoader.java)
  creates a new `ApkAssets` for the base and each split. Its original `close()` closes
  only the cached `AssetManager`.
- [SplitAssetDependencyLoader](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/com/android/internal/pm/split/SplitAssetDependencyLoader.java)
  likewise creates private assets but shares references to them across parent/child
  dependency arrays. Its original `close()` closes only the cached managers.
- [AssetManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/content/res/AssetManager.java)
  intentionally does not own every contained `ApkAssets`: system assets and other
  resources may be shared. Closing its final reference destroys the manager, not the
  independent asset objects.
- [ApkAssets](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/content/res/ApkAssets.java)
  has a synchronous explicit `close()`, which closes its string block, invokes native
  destruction and clears `mNativePtr`. Its finalizer is a fallback. It does not implement
  `AutoCloseable` in this pin.
- [ParsingPackageUtils](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/com/android/internal/pm/pkg/parsing/ParsingPackageUtils.java)
  closes each manifest XML parser before its outer `finally` closes the split loader.
  Parsed package values do not transfer ownership of the loaders' native resources.
- [ApkLiteParseUtils](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/content/pm/parsing/ApkLiteParseUtils.java)
  already explicitly closes its own temporary `ApkAssets` after closing the XML parser;
  that path needs no change.

Patch `0783-parser-apk-assets-lifetime.patch` gives each of the two loaders a private
creation list. Each successful load is recorded immediately, including assets acquired
before a later split/configuration fails. An ownership-list insertion failure also closes
the just-created asset. Setup failures close the unpublished local manager before
propagating their original exception.

On loader close, all published managers close first, followed by exactly the assets the
loader created. The creation list avoids closing one parent asset once for every alias
in a dependency array. The implementation never takes ownership from
`AssetManager.getApkAssets()` and never closes borrowed system/provider assets.
Repeated close remains safe. Neither global `AssetManager.close()` nor native
open/truncate semantics change.

The framework's existing sparse checkout must include these two exact source files before
patch application. `aosp.lock` adds their paths without changing the Android tag or removing
other sparse inputs; `tools/fetch-aosp.sh` otherwise applies patches before the later Java
source expansion could make them available.

The installer no longer tries five cycles of `System.gc()`, finalization and sleeps when
deleting a retired code generation. It performs the real no-follow deletion and reports
failure. A generation still held by another owner remains eligible for the existing
startup cleanup of unregistered generations. Startup cleanup and installation/registry
publication rules are unchanged.

The exact unmodified loader inputs checked by the host test are:

| Android source | SHA-256 |
| --- | --- |
| `DefaultSplitAssetLoader.java` | `4ec51dfe62ef3b5c19b564a9e8a0cc3128c6e9639ef6de39f6b4081509e011da` |
| `SplitAssetDependencyLoader.java` | `8666e61d7c5f5739d53d95ec9c4713e281d0ad65cd9e5fca0c398f9eddb995ea` |

## Regression evidence

`tools/test_parser_asset_lifecycle.py` applies the actual production patch to those
hash-checked sources and compiles the real loader classes and the ART fixture against
the recovered Android framework/core. It then runs the real framework Java on JDK 21,
with the fixture in a separate child class loader and `-Xcheck:jni` enabled. Its narrow
host JNI boundary owns real Linux file descriptors/mappings, records native owner
references, rejects asset destruction while a manager still refers to it, and injects
configuration/asset-assignment failures. It does not decode Android resource tables or
implement the Windows mapping backend.

Local results on this revision:

| Validation | Result |
| --- | --- |
| Actual Android core/framework compile, Java source/target 17 | PASS |
| Standalone ART fixture D8, minimum API 36 | PASS, 11,268 bytes |
| Real Java loader ownership and source rewrite assertions | 88 PASS |
| Host JNI setup-failure/liveness assertions | 26 PASS |
| Combined host run with `-Xcheck:jni` | 114 PASS; no JNI warnings/errors |
| Unmodified pinned-loader negative control | FAIL at `Loader left a private APK mapping alive after close` |
| Independent Java and native peer runs/reviews | PASS / clear |

The negative control retains private assets obtained from the actual manager before
closing the old loader. It observes their still-live native pointers. It fails before
any inspection of the newly added ownership-list field, so a missing test hook does not
explain the failure. No test asks for garbage collection.

The common assertions cover default-loader cache reuse; the real isolated dependency
graph and shared parent-asset identity; successful close and repeat close; later-split
I/O failures; failure of a base-plus-config load before any manager is published; and
preservation of borrowed system assets. Host-only failure injection additionally checks
both setup stages, including an asset-assignment failure after the manager has retained
the resources.

`ParserAssetLifecycleTest` is also a mandatory Windows track 94 test using the existing
`split-package-fixtures.zip`. In ART it parses the real compiled APKs, opens their binary
manifests, keeps all private asset objects strongly reachable through loader close,
checks actual native pointers, truncates the source files to four bytes and restores and
compares their full bytes. The partial-load cases retain assets before closing the loader;
the fixture never closes them on its behalf. The existing complete installer/split test
continues to run immediately alongside this witness with its original source update.

Invocation:

```text
gesher.services.ParserAssetLifecycleTest <split-package-fixtures.zip>
```

Required marker: `ParserAssetLifecycleTest all passed:` and a successful process exit.
Both actual Windows execution and the unchanged installer regression must pass on the
new source before claiming the Windows failure is fixed in CI. They are pending at the
time of this report; the earlier `f0d0479f` outcomes are not results for this patch.

Reproduction of the local evidence:

```sh
python3 tools/test_parser_asset_lifecycle.py \
  --aosp-base <unmodified-frameworks-base> \
  --framework-classes <framework-classes.zip> \
  --core-classes <exploded-core-module> \
  --java-home <jdk-21> --r8 <r8.jar>
```

Add `--unpatched` to run the original pinned loaders as the negative control. The only
local compiler warnings were the existing missing `SystemApi.Client` annotation enum
metadata; the compile, D8 and runtime checks returned success for the corrected source.
