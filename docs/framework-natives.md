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

Gesher-owned: an `android.app.Activity` stub (`getDlWarning` returns null)
and a small `android.os.Process` set (thread priority, argv0, CPU time),
grown as `UnsatisfiedLinkError`s appear.

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
- New: libcutils (Windows set plus host trace and ashmem), libutils and
  libutils_binder (Windows set plus `Looper.cpp`), libandroidfw (without the
  PNG files), libincfs-utils (`map_ptr`), the `android.content.res.flags` C++
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

## POSIX

epoll and eventfd are done. Still needed: `pthread_atfork` (a no-op
returning 0; `ProcessState::init` aborts otherwise), `fcntl(F_DUPFD_CLOEXEC)`,
and later ashmem over `memfd_create`. `/dev/binder` must fail with ENOENT.

## Order of work

Each step has a test run in CI on Windows.

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
