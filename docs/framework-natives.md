# Framework natives for a headless Activity

What `libandroid_runtime` must provide for an Activity to go through
attach, create, start and resume (Phase 2), and the order of work. Surveyed
against android-16.0.0_r4; Ravenwood's native loader and policy files were
the starting checklist.

## Registrations

Needed, all from `frameworks/base/core/jni`:

| register_* | Native deps |
|---|---|
| `android_os_SystemProperties` | libbase property store (shared with ART) |
| `android_os_SystemClock` | libutils |
| `android_util_Log`, `android_util_EventLog` | liblog |
| `android_os_Trace` | libcutils trace; perfetto replaced by a stub (all tags off) |
| `android_os_MessageQueue` | libutils `Looper.cpp` (epoll, eventfd) |
| `android_content_res_ApkAssets`, `android_content_AssetManager`, `android_content_StringBlock`, `android_content_XmlBlock` | libandroidfw, libziparchive |
| `android_os_Binder`, `android_os_Parcel` | libbinder |
| `android_text_AndroidCharacter` | ICU4C |
| `com_android_internal_util_VirtualRefBasePtr` | libutils |
| `android_util_jar_StrictJarFile` | libziparchive |

Gesher-owned: an `android.app.Activity` stub (`getDlWarning` returns null),
a small `android.os.Process` set (thread priority, argv0, CPU time)
and `android.os.Debug`'s native heap counters (the process heap's
`HeapSummary`) and memory accounting (`getMemoryInfo`, `getPss`, `getRss`
from the process working set, `QueryWorkingSet`; `getMemInfo` from
`GetPerformanceInfo`),
grown as `UnsatisfiedLinkError`s appear. The lifecycle also reached, and
Gesher stands in for: `android.os.PerfettoTrace` (categories never enabled;
MessageQueue builds them). Phase 2 also stood in for
`android.graphics.ColorSpace$Rgb` and hwui's `RenderNode`, `Paint`, `Matrix`
and `Path`; Phase 3 replaced those by hwui's real natives
(docs/graphics.md), and the generated zero-answer tables
(`compat/framework/gen_device_standins.py`) now cover the device-only
classes: `SurfaceControl`, `SurfaceSession`, `BLASTBufferQueue`. See
[0013](decisions/0013-framework-natives.md).

Left out until views (hwui, `android.graphics.*` and Surface came with
Phase 3, docs/graphics.md): input,
SQLite, CursorWindow, minikin, FileObserver, HwBinder. ART resolves natives
lazily, so an unregistered class fails only when one of its natives is
called. The first target stops at `performResume`, before `makeVisible`.

`HostRuntime.cpp` is not reused (its table references every host source
and hwui). `compat/framework/android_runtime.cc` holds the host
`AndroidRuntime` statics and a fixed table, registered as the builtin
`libandroid_runtime.so` the way `compat/libcore/builtins.cc` does.

## APK integrity capabilities on Windows

`compat/framework/apk_integrity.cc` registers all five `IncrementalManager`
native methods. Windows files are not IncFS files: the feature and v2 probes
return false, path/fd probes return false and the raw IncFS signature query
returns null. This lets `PackageManager.getPackageArchiveInfo` use AOSP's
ordinary certificate verification instead of throwing `UnsatisfiedLinkError`
before it can examine an APK. It does not return certificates or declare an
APK verified.

The same file registers `VerityUtils`' four filesystem native methods, reached
when the v4 verifier finds an `.idsig` sidecar. Existing files have no fs-verity
attribute or kernel digest, and enabling it returns `EOPNOTSUPP`, never success.
Missing paths and invalid descriptors retain their actual lookup errors. The
enable methods return positive errno and stat/measure return negative errno;
measure never fills the caller's digest buffer. Framework patch 0788 also
reports `isFsVeritySupported()` as false: Android's initial SDK version does
not imply a Windows filesystem capability.

The v4 verifier is unchanged: no IncFS metadata causes a sidecar lookup, and a
sidecar cannot be accepted without a kernel-enforced fs-verity digest. AOSP's
fallback rules still apply to embedded v3/v2 or JAR v1 signatures. Neither an
APK certificate, signature scheme nor signing lineage is manufactured here.

Contract sources (Android 16):
[IncrementalManager JNI](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/jni/android_os_incremental_IncrementalManager.cpp),
[v4 verifier](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/util/apk/ApkSignatureSchemeV4Verifier.java),
[VerityUtils JNI](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/jni/com_android_internal_security_VerityUtils.cpp).

## Debug memory accounting on Windows

The `android.os.Debug` memory natives use the Android 16 JNI signatures and kB
units. `getMemoryInfo` and `getPss` sample resident working-set pages;
`getRss` reads the process working-set size. USS counts exclusive resident
pages, and PSS apportions shared pages using the Windows share count. Windows
caps that count at seven, so widely shared pages can make PSS an overestimate.
Pages outside the ordinary working set, including large-page allocations, are
not represented by this snapshot.

Windows working-set protection describes permissions, not the Linux dirty bit
or file backing. The private/shared dirty and clean split is a permission-based
estimate. The calling process's dalvik and native categories use live Java heap
and process-heap allocation estimates, capped by the sampled private dirty
budget; these are not measurements of each heap's resident pages. The remainder
is `other`, and a different process is reported entirely as `other`.
Per-mapping `otherStats`, swappable PSS, swap PSS and graphics memtrack values
are unavailable and remain zero; `hasSwappedOutPss` is false. A read-only page
alone is not evidence that its contents can be discarded and read from a file.

`getMemInfo` reports measured physical total and immediately available memory
in `MEMINFO_TOTAL` and `MEMINFO_AVAILABLE`. Other Linux-specific slots are
unsupported and zero. In particular, subtracting Windows `SystemCache` from
`PhysicalAvailable` does not produce Linux free memory: the former includes
the system working set as well as standby pages. Paged kernel pools do not
identify reclaimable slab, and system commit headroom does not measure pagefile
use. See Microsoft's [counter definitions](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-performance_information)
and [working-set page layout](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-psapi_working_set_block).

Working-set snapshots have a 64 MiB temporary-buffer limit and at most nine
query attempts. This holds roughly 32 GiB of resident memory with 4 KiB pages.
Allocation failure, an oversized/inconsistent snapshot, or an unreadable
process returns false/zero without modifying caller outputs. The allocation
path does not throw C++ exceptions. Partial process output arrays are accepted,
and extra array slots stay unchanged. `getMemInfo` requires at least
`MEMINFO_COUNT` entries and throws on a missing output or failed system query.

## Libraries

- Already built: libbase, liblog, libziparchive, zlib, libnativehelper, ICU4C.
- New: libcutils, libutils, libutils_binder and libandroidfw in full: each
  builds its Android.bp device source set (libcutils with host trace and
  ashmem, libutils with its pthread threading under `GESHER`), plus
  libutilscallstack (CallStack over a Windows x64 unwinder) and libpng; AOSP's
  own gtests of these libraries run on Windows. Details, exclusions and CI
  runs: [core-libs.md](core-libs.md).
- Also new: libincfs-utils (`map_ptr`), the `android.content.res.flags` C++
  aconfig, libbinder (built without `__ANDROID__`, so a missing
  `/dev/binder` only logs), a `hwbinder/IPCThreadState.h` shim.

## Source patches

1. Hand-written JNI tables register `(void*)fn`, so ART would call
   Microsoft-ABI functions with the System V ABI. A generated
   frameworks/base patch turns each into `GESHER_JNI(fn)`, which expands to
   `NATIVEHELPER_SYSV_JNI_ENTRY(fn)` under `GESHER`.
2. `core_jni_helpers.h` drops `CRITICAL_JNI_PARAMS` off-device; ART honours
   `@CriticalNative`, so it applies under `GESHER` too.
3. `android_util_AssetManager.cpp`'s `_WIN32` branches read
   `FileDescriptor.handle`, which Android's libcore lacks; they are skipped
   under `GESHER`.
4. libandroidfw (frameworks/base 0003): two `_WIN32` blocks that redefine
   `O_CLOEXEC` and the byte-order macros, which gesher-base has.
5. libbinder (frameworks/native 0001): LLP64 size assertions, libbase's
   `Pipe`, `uid_t`; libcutils' `sockets.h` takes its Unix branch
   (system/core 0002), as gesher-base's sockets are descriptors.

## POSIX

epoll, eventfd, `pthread_atfork` (a no-op returning 0; `ProcessState::init`
aborts otherwise), `fcntl(F_DUPFD_CLOEXEC)` and an ENOENT for `/dev/binder`
are done; gesher-base also gained `<syslog.h>` and the `<linux/*.h>` uapi
pieces libbinder includes. Later: ashmem over `memfd_create`.

## Order of work

Each step has a test run in CI on Windows (`tests/windows/run-tests.ps1`;
the tests are in `tests/dex-natives`, see
[0013](decisions/0013-framework-natives.md)).

1. libcutils, libutils (with Looper), libutils_binder.
2. The android_runtime skeleton and the patches; SystemProperties,
   SystemClock, Log, EventLog, Trace, AndroidCharacter, VirtualRefBasePtr,
   plus a `@CriticalNative` ABI canary.
3. MessageQueue: main Looper, `Handler.postDelayed`, a HandlerThread.
4. libandroidfw and the asset natives: `Resources.getSystem()` strings,
   theme attributes, a test APK's manifest.
5. libbinder and Binder/Parcel: Parcel round trip, an in-process AIDL call.
6. StrictJarFile; Process additions.
7. `gesher-app`: props from a build.prop, stand-in services through
   `ServiceManager.initServiceCache` (activity, activity_task, package,
   window, display), then ActivityThread, LoadedApk, the app and activity
   contexts, `attach`, `performCreate`, `performStart`, `performResume`.
   `SystemServiceRegistry`'s mainline initializers need real or no-op code.

## Software JNI contract regressions

The 2026-11-05 candidate also binds MemoryFile, MemoryIntArray, Scrypt,
filesystem LocalSocketImpl streams and CharsetUtils. Four boot-framework
fixtures verify bytes, errors and ownership, including Parcel memory readers,
RFC 7914 answers, stream half-close and real fast UTF stream callers. The
[contract report](research/framework-jni-contracts-2026-11-05.md) records the
passing candidate identities and exact remaining ashmem protection, local
namespace/ancillary and raw-buffer limits. The bounded standalone runner is
`tests/windows/run-framework-jni.ps1`; the main Windows suite includes these
fixtures as well.
