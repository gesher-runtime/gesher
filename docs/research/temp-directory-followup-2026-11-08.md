# Temporary-directory initialization follow-up (2026-11-08)

## Confirmed runtime failure

The focused Windows 2025 job for source
`8af8d702bef18a7a6caead597595542b3aa41183` reached the real package-query,
activity-options, split-installation, and URI-grant fixtures, then failed at
their ordinary `Files.createTempDirectory` calls. Each stack ended in
`TempFileHelper.create` and `UnixFileSystemProvider.createDirectory` with
`NoSuchFileException: /tmp/gesher-…`. The source and artifact receipt checks
had already passed. This was a shared runtime default, not four independent
fixture defects.

Evidence: [focused Windows job and complete log](https://github.com/gesher-runtime/gesher/actions/runs/42450333175/job/128639822766).
The same unqualified temporary-directory API is used by production
`Installer` when extracting an APK archive and by `IconApks`. Creating a
fixture-only directory would leave those callers broken.

## Pinned Android contract

All Android comparisons use `android-16.0.0_r4`:

- [`System.java`](https://android.googlesource.com/platform/libcore/+/android-16.0.0_r4/ojluni/src/main/java/java/lang/System.java)
  builds unchangeable defaults from VM properties, including explicit `-D`
  options, then adds a changeable `/tmp` default only when no explicit
  `java.io.tmpdir` was supplied. `setProperties(null)` reconstructs the
  changeable defaults. Named `getenv(String)` directly calls `Libcore.os`;
  the existing earlier initialization already uses it for `ANDROID_ART_ROOT`.
- [`ActivityThread.setupGraphicsSupport`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ActivityThread.java)
  selects an application's cache with `System.setProperty("java.io.tmpdir", …)`
  and `Os.setenv("TMPDIR", …)` before application code runs. It skips the
  `android` system package. A generated `-Djava.io.tmpdir` would therefore
  incorrectly freeze the standalone default against the app-cache update.
- [`TempFileHelper`](https://android.googlesource.com/platform/libcore/+/android-16.0.0_r4/ojluni/src/main/java/java/nio/file/TempFileHelper.java)
  caches the default directory when the class is initialized and performs
  actual file/directory creation. Tests must use fresh VMs and change the
  app-cache property before the first temporary-file call.
- [`dalvikvm.cc`](https://android.googlesource.com/platform/art/+/android-16.0.0_r4/dalvikvm/dalvikvm.cc)
  stops interpreting VM options at the main class, consumes `-cp` and
  `-classpath` operands, and handles `-XXlib:` separately. The entry adapter
  follows those boundaries when detecting an explicit temp property.
- [`UnixFileSystem`](https://android.googlesource.com/platform/libcore/+/android-16.0.0_r4/ojluni/src/main/java/java/io/UnixFileSystem.java)
  and [`UnixPath`](https://android.googlesource.com/platform/libcore/+/android-16.0.0_r4/ojluni/src/main/java/sun/nio/fs/UnixPath.java)
  collapse repeated slashes. A raw `//server/share` return path consequently
  cannot preserve UNC identity through the ordinary Java file APIs.

The downloaded source SHA-256 values are:

| Source | SHA-256 |
| --- | --- |
| `System.java` | `1e684563c9e325cc3fa0d3470a9f9663b56d7237d3bfa7123823e6c851f6fa17` |
| `ActivityThread.java` | `394e70067e4b4b675f7d38f924137837cf01daf062422217084b495e0efbec7a` |
| `TempFileHelper.java` | `2cd034045c3a599491620f3a9b4b471930d0348680c8ffeec184a7ae2a5ccfda` |
| `dalvikvm.cc` | `840583bf5bc9b55e5b1ddb57ff3642af5d4ecaf9040a2c84fa374be3fb6b9259` |
| `UnixFileSystem.java` | `51b144cc72e90e44b7e6dced9360e269bccf1ddfbbcddd1547ac28212c846d22` |
| `UnixPath.java` | `2de5d5987d06a521018acde3e1042b005a0cb73c8e748bf593b746915f5dc497` |

## Runtime repair

`compat/art/dalvikvm_main.cc` initializes the default before entering ART.
An explicit `TMPDIR` remains a guest path exactly as supplied. A genuine VM
`-Djava.io.tmpdir=…` bypasses automatic initialization; an application argument
or classpath operand with the same spelling does not. No VM option is
inserted, removed, or reordered.

Without either override, the adapter obtains the Windows temporary path with
the wide-character `GetTempPathW` API. A drive path becomes
`/mnt/<drive>/…`, including when it happens to be on the current drive. This
avoids sending a user-local C: directory to the runtime's D: drive. A normal
or extended UNC path uses the shared `gesher/unc_paths.h` conversion to
`/mnt/unc/<server>/<share>/…`, which survives Java normalization. Native
filesystem and Zygote conversion support for that namespace is a companion
change; the adapter uses the same helper rather than a second parser.

The adapter publishes UTF-8 to the narrow CRT environment read by libcore
and publishes the original UTF-16 to the operating-system environment used
for process inheritance. Publication errors stop startup rather than
pretending that the selected directory was installed. This contract covers
those two environment views, not every independently cached CRT wide
environment table. The [`_putenv_s` documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/putenv-s-wputenv-s)
and [`CRT environment documentation`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/environ-wenviron)
explain the distinct environment representations and the special removal
meaning of an empty `_putenv_s` value; an explicitly empty override is
therefore preserved without calling that removal operation.

`patches/libcore/0780-windows-temp-directory.patch` changes only the mutable
default: use named `TMPDIR` when present, otherwise retain `/tmp`. Explicit
`-D` immutability and `ActivityThread`'s app-cache selection remain intact.
Resetting the properties reads the current native `TMPDIR`, including after
the app-cache update.

The adapter maps the selected path without eagerly creating a directory or
checking permissions. [`GetTempPathW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppathw)
selects a path without establishing its existence or access rights. Actual
`File`/`Files` calls retain the real missing-directory, wrong-file-type, or
permission error. This also allows `ActivityThread` to select a valid app
cache before the first temp operation when the host default is unavailable.
There is no global drive-root `/tmp` directory and no silent alternate path.

## Regression evidence

The portable entry test includes the actual production adapter and models
only Windows/CRT boundary calls. It passes **100 checks** for drive identity,
Unicode and spaces, extended paths, UNC roots, long paths, explicit overrides,
option boundaries, failure propagation, and preserving unavailable defaults
without eager I/O. The same harness against published `8af8d702` fails at its
first default-directory identity assertion. This is a regression witness for
entry control flow; it is not Windows runtime execution.

Both Windows x64 and ARM64 compilers successfully compile the complete
adapter with the production C++20/global definitions and ART warning set,
plus warnings-as-errors. The x64 check includes the current SSE4.2/POPCNT
flags. The new header is available through `gesher_posix`'s public include
directory and the existing global AOSP include setup. These are object
compilation checks, not full ART linkage or runtime results.

The libcore patch applies cleanly to the pinned source. Patched `System.java`
and `TempDirectoryContractTest.java` compile against the actual Android core
module with JDK 21, target 17, and the repository's inline string-concatenation
mode. The PowerShell orchestration parses successfully; its process exit,
pipe EOF, cancellation, handle closure, and cleanup waits are bounded using
the existing shared reader-cleanup helper.

The Windows runner now invokes ten **fresh VM** scenarios, preserving ordinary
unqualified `Files.createTempDirectory`, `Files.createTempFile`, and
`File.createTempFile` calls:

| Scenario | Required result |
| --- | --- |
| Unicode Windows default, with a fake `-D` after the main class | Real create/write/read/delete in the selected host directory |
| Explicit Unicode guest `TMPDIR` | Override preserved; real I/O at that path |
| Explicit `-D` and a missing Windows default | Immutable property and real I/O at the explicit directory |
| App-cache update before first temp call, with a missing host default | Mutable property and native environment update; real I/O in cache |
| Missing Windows default | Real missing-directory error |
| Missing explicit `TMPDIR` | Same error; no fallback |
| Missing explicit `-D` | Same error; no fallback |
| Ordinary file as Windows default | Real I/O error; existing bytes unchanged |
| Ordinary file as explicit `-D` | Same error; existing bytes unchanged |
| Directory with a real creation-denying ACL | Access-denied error; original ACL restored during cleanup |

Successful local-drive completion prints
`TempDirectoryBootstrapTest all passed: 10 fresh VM scenarios`. The fixture
also checks `setProperties(null)`, confirms the directory on disk, verifies
exact temporary-file parents and bytes, rejects duplicate creation without
overwriting, and checks deletion. It does not rewrite Java's cached temp
helpers. The runner logs host/runtime drives without assuming that every
manual machine uses different drives. A separate companion UNC fixture
creates a private SMB share and runs the same ordinary default-temp Java
operations in another fresh VM over the UNC namespace.

At source freeze these newly added Windows scenarios and the companion SMB
case have **not yet executed in CI**. The passing local results establish
source application, target compilation, and modeled entry behavior. Actual
Windows execution must establish temp I/O, ACL/error handling, process cleanup,
and the previously blocked package/URI fixtures. This repair does not claim
that arbitrary JNI embedding paths or all application features have been
validated.
