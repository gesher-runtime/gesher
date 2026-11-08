# Android compatibility and crash-contract audit, 2026-11-08

The inspected upstream baseline is
`a4dd7242cfed1a08cd8761ae896d63faf7b8cf63` (after PR #7). Existing
November 5 fixes were checked before selecting additional work. The audit
covers current Java service/value implementations, package parsing and
queries, native allocation/OS adapters, ELF failure paths and the existing
compatibility inventories. It does not establish that every possible APK
runs, or that every Android API behaves correctly.

Android's [CTS overview](https://source.android.com/docs/compatibility/cts)
separates signature checks from behavioral tests. This distinction matters
here: an existing class can lack an overload, a service can inherit a null
Default answer, and a resolved native function can still corrupt memory.
The reference release for this repair is the project's pinned Android 16 r4,
rather than an unrelated newer platform API.

## Confirmed findings repaired

| Area | Failure in the inspected source | Repair and regression evidence |
| --- | --- | --- |
| Native aligned allocation | `size + alignment` could overflow and return storage smaller than requested. A failed aligned `realloc` freed the original block, leaving an ordinary recovery path with freed memory. | Check arithmetic and retain storage/ownership until replacement allocation succeeds. Production-source ASan/UBSan regressions and shared ELF tests cover overflow, contents, retries and ownership. |
| Remaining allocator contracts | `reallocarray` was not exported; zero-size aligned `realloc` and zero-dimension `calloc` differed from bionic; `posix_memalign` failure changed errno. | Add checked `reallocarray`, correct zero-size behavior, and preserve errno/output on `posix_memalign` failure. See the [allocator report](native-allocation-contracts-2026-11-08.md). |
| Native group queries and file locks | `getgroups` could write through a null output pointer and accepted negative sizes. `lockf` returned success without acquiring or releasing a range. | Validate group arguments. Route nonblocking lock/unlock/test to the existing fcntl record-lock layer. Blocking `F_LOCK` explicitly returns `ENOSYS`; it does not claim to acquire a lock. See the [system-adapter report](native-system-contracts-2026-11-08.md). |
| Socket control data | `sendmsg` silently discarded nonempty ancillary data while sending the payload; batched sends could likewise claim successful control delivery. | Reject unsupported control messages before transmission, with `EOPNOTSUPP`. Real socket peers verify that no forbidden payload is sent and that batch counts stop at the failed message. This does not implement descriptor transport. See the [socket report](native-socket-control-contracts-2026-11-08.md). |
| DNS-SD manager APIs | Three public request overloads were absent; stop callbacks used the wrong executor, listener equality could conflate registrations, and rejected submissions could leave unusable state. | Restore the descriptors and repair request ownership/executor handling. The actual production classes pass controlled-executor tests; the old class fails descriptor lookup. See the [NSD report](nsd-contracts-2026-11-08.md). |
| Audio queries | Actual framework wrappers dereferenced null communication-device and head-tracking arrays. Other public non-null lists also inherited null Defaults. | Return consistent empty/unavailable capability values and test the real ContextImpl managers. See the [audio report](audio-query-contracts-2026-11-08.md). |
| Subscription queries | Generated defaults reported the valid identifier `0` despite an empty subscription inventory, inviting initialization paths that assume a SIM exists. | Return the pinned framework's absent subscription/slot identifiers, preserve its distinct unmapped-phone sentinel, and provide non-null empty ID arrays. The host fixture passes 26 production-service assertions; public manager wrappers join the Windows fixture. This is a reproduced contract defect, not a reproduced third-party APK crash. See the [subscription report](subscription-contracts-2026-11-08.md). |
| Package metadata | `getPackageInfoVersioned` returned null for installed ordinary packages, canonical-name mappings returned null arrays, and parsed manifest properties were discarded by queries. | Delegate ordinary package lookup to existing metadata, preserve name-array positions, and expose the parsed application/component property values. Test the actual parser, generated AIDL proxy/stub, Parcel transport and ApplicationPackageManager, including unknown-user/package and removal cases. |
| Installed split APKs | Installation copied splits but parsed only the base manifest and synthesized names from filenames. Feature components, dependency graphs and class-loader policy were missing. | Parse validated installer-owned clusters through AOSP, preserve the isolated loading flag, and use the full graph during install/reload, shortcuts and dexopt context selection. Reject malformed staging before replacing the running installation. Real feature DEX/resource fixtures cover dependencies and failed-update preservation in Windows CI. See the [split-package report](package-split-contracts-2026-11-08.md). |
| Proxy initialization | The documented nullable copy constructor dereferenced null. A valid PAC description with its initial local port of -1 threw while converting to a Java proxy. | Accept the empty snapshot and return the framework's nonthrowing conversion result while a local port is unavailable. Reject null PAC URIs as Android does. Host value tests and Windows Parcel tests cover pending, ready and absent proxies. |

The proxy reference is
[pinned AOSP ProxyInfo.java](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/ProxyInfo.java).
The implementation retains its existing normalized exclusion-list representation
and local parcel encoding; these checks do not establish AOSP wire-format
interoperability or complete direct-host validation.

The package references are pinned framework
[ApplicationPackageManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/app/ApplicationPackageManager.java),
[ComputerEngine](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java),
and [PackageProperty](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageProperty.java).
A VersionedPackage version selects static shared-library providers on AOSP;
it does not require the version of an ordinary installed app to match.
The patch therefore does not invent a new mismatch error for ordinary apps.

## Validation inputs and boundaries

The Java compile inputs come from successful repository CI run
[42425141954](https://github.com/gesher-runtime/gesher/actions/runs/42425141954),
source `a6749ab7655398a51ebd719d6d9b129d87e2888e`. The framework inputs
(`aosp.lock`, patches, framework/generator/host build sources and core-module
construction) were compared with the inspected baseline and are unchanged.

| Input | SHA-256 |
| --- | --- |
| GitHub artifact 13595267948, framework/core compile inputs | `e27b851b2cd2ac52481c341ea102e6f959a10dc528373e7cec5a35ad6668ed79` |
| Inner framework classes ZIP | `8d8976799d323066c360c174f60c83a8ce08604c46d4864b0c25e70000a46020` |
| Inner core module ZIP | `ec67a47be8680d60687f03e80c2e50e386f2f4a1298cd8de3289adebedfebb49` |
| GitHub framework artifact 13595730967 | `56341de876dc1b9111068823d3b7b62c9021886b8a05388f9b3e801a273d9adb` |
| Reference framework-res.apk | `1e79a1235f26794e1c0d0bf6c186bee90cd8a98a57f8e47c5144c954e3d14a3f` |

The source combination passes Java compilation and D8 generation for the app
and service tests. The local app compilation uses pinned core/java sources
for compat-table generation and does not assemble all optional AOSP source
modules. Its output is a compile-only check, not a distributable or installed
runtime. The normal CI build regenerates its full inputs from the source tree.

The NSD host fixture passes 37 assertions, the proxy host slice passes 20,
and the subscription host fixture passes 26.
They execute the production classes against the actual framework/core compile
inputs, without replacement Android API stubs. The audio report distinguishes
actual wrapper failures from the supplemental host diagnostic that bypasses
Windows initialization. The complete package fixtures, successful communication/
subscription manager paths and native Parcel checks require the Windows ART runtime.
The split-package builder also passed aapt2, javac and D8 compilation; its
binary manifests were inspected. Building those APKs is not equivalent to
executing the installed feature class-loader/resource assertions.

The native allocator, OS-adapter and socket tests pass 35, 41 and 40 assertions
respectively (116 total). They compile the corresponding production sections
and exported registrations verbatim, with an explicitly described
host envelope. Their shared fixtures also join the existing native ELF suite
for Windows x86_64 and translated ARM64. Local ASan/UBSan runs disable only
LeakSanitizer because this environment cannot inspect the required process
state. They are not Windows execution, cross-ABI proof or a leak-check result.
The lightweight CI gate runs all three sanitizer harnesses before the larger build.

## Material compatibility work still open

These current-source limits are separate from the repaired defects:

- **Service and Java module coverage.** The source inventory still finds many
  inherited Binder Default methods. These are review candidates, not a count
  of crashes: some are optional, some are handled elsewhere, and some need
  valid callbacks or object values. Bluetooth's local manager has no adapter
  and its API surface is partial. An absent hardware feature does not ensure
  that every SDK callback/value class required while loading an app exists.
- **Real discovery and audio policy.** The NSD implementation has no mDNS
  backend. Communication-route selection, spatialization and Android recording
  inventories remain absent. Empty results prevent the identified wrapper
  crashes; they do not implement those capabilities.
- **Native readiness and descriptor transport.** `compat/posix/src/epoll.cc`
  still explicitly treats `EPOLLET` as level-triggered. Socket ancillary-data
  transport, including SCM_RIGHTS, is not implemented. Blocking record-lock
  waits remain a separate backend obligation.
- **Native exports and ABI behavior.** The platform soname table is conditional
  on implemented probe symbols. A named library or successful symbol lookup
  does not establish every export, layout, callback or ownership contract.
  `compat/elf/elf_loader.cc` still aborts on invocation of an unresolved native
  function; required missing imports can therefore still terminate an app.
- **Integration beyond these fixtures.** Full ARM64 instruction/signal/JIT
  behavior, media/graphics backends, physical devices and wider application
  lifecycle combinations require their own evidence. Existing implemented
  slices are preserved; older audit counts are not current failure totals.

The existing [central map](systematic-compatibility-2026-11-05.md) and
[inventory guide](../compatibility-audit.md) provide the broader navigation.
The Java/NDK/service/JNI inventories measure different overlapping surfaces;
their numbers must not be added or turned into an Android-support percentage.
