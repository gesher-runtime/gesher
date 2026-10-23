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

Gesher-owned: an `android.app.Activity` stub (`getDlWarning` returns null)
and a small `android.os.Process` set (thread priority, argv0, CPU time),
grown as `UnsatisfiedLinkError`s appear. The lifecycle also reached, and
Gesher stands in for: `android.os.PerfettoTrace` (categories never enabled;
MessageQueue builds them), `android.graphics.ColorSpace$Rgb`
(DisplayManagerGlobal creates the named color spaces), and hwui's
`RenderNode`, `Paint`, `Matrix` and `Path`, which View and Drawable
constructors call: their tables are generated from hwui's JNI sources
(`compat/framework/gen_hwui_standins.py`) and answer zero, or a fresh
object, as nothing is drawn yet. See
[0013](decisions/0013-framework-natives.md).

Left out until views: hwui and `android.graphics.*`, Surface, input,
SQLite, CursorWindow, minikin, FileObserver, HwBinder. ART resolves natives
lazily, so an unregistered class fails only when one of its natives is
called. The first target stops at `performResume`, before `makeVisible`.

`HostRuntime.cpp` is not reused (its table references every host source
and hwui). `compat/framework/android_runtime.cc` holds the host
`AndroidRuntime` statics and a fixed table, registered as the builtin
`libandroid_runtime.so` the way `compat/libcore/builtins.cc` does.

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
