# 0013 — The framework's natives, linked in

**Status:** accepted, 2026-10-23

## Context
framework.jar's classes call into `libandroid_runtime`, which on a device is
loaded by the Zygote and registers some 200 JNI tables written by hand in
`frameworks/base/core/jni`. The headless lifecycle
([0012](0012-headless-framework.md)) needs a few of them
([the plan](../framework-natives.md)), and their libraries: libcutils,
libutils, libandroidfw and libbinder. As with libcore's natives
([0011](0011-libcore-natives.md)), there is no ELF loader, and ART enters
natives with the System V ABI ([0006](0006-managed-abi.md)).

## Decision
- **One built-in library.** `compat/framework/android_runtime.cc` holds the
  `AndroidRuntime` statics the natives call (`getJavaVM`, `getJNIEnv`,
  `registerNativeMethods`), a fixed registration table, and
  `JNI_OnLoad_android_runtime`, registered with gesher-base's `dlopen` as
  `libandroid_runtime.so`, so `System.loadLibrary("android_runtime")` finds it.
  `AndroidRuntime.cpp` is not used (its table names every host source and
  hwui). The registrations run in `AndroidRuntime.cpp`'s order, logging,
  clock and properties first: others initialize classes (`StrictMode`, from
  Binder's) whose static initializers call those. Each registration names a
  class it needs; when that class is not on the class path the registration
  is skipped with a log line. With framework.jar every class is there; the
  tests load stand-ins for a few (below).
- **The sources as Soong lists them**, built in `cmake/aosp/framework.cmake`,
  `system.cmake` and `binder.cmake`: libcutils (the Windows set with the host
  trace and ashmem, and the Unix socket sources, as gesher-base's sockets are
  descriptors), libutils with `Looper.cpp` over gesher-base's epoll and
  eventfd, libandroidfw (without the PNG files and CursorWindow), libincfs-utils'
  `map_ptr`, libbinder's host configuration with kernel IPC, and the
  `core/jni` sources of the registrations in use. libandroidfw, libbinder and
  the JNI sources compile as C++23: `cutils/trace.h` includes C's
  `<stdatomic.h>`, which libc++ allows next to `<atomic>` only from C++23 on
  (bionic's allows it always), and libandroidfw asks for `gnu++2b` itself.
- **System V entries from the tables.** `tools/jni-sysv-patch.py` generates
  frameworks/base patch 0002: each `(void*) fn` in a JNI table becomes
  `GESHER_JNI(fn)`, a System V thunk under `GESHER` (`core_jni_helpers.h`,
  patch 0001); a function already declared `JNICALL` becomes
  `GESHER_JNI_SYSV(fn)`, itself. Patch 0001 also keeps `CRITICAL_JNI_PARAMS`
  empty under `GESHER`, as ART honours `@CriticalNative`. Tables written with
  `NATIVE_METHOD` (StrictJarFile) get their thunks from libnativehelper's
  patch 0002.
- **Gesher-owned natives** where the real source is device plumbing:
  `android.app.Activity.getDlWarning` (no linker warnings) and a part of
  `android.os.Process` by the framework's own names (`setThreadPriorityNative`,
  `setArgV0Native`, thread and process groups accepted and ignored, signals
  through gesher-base's `kill`, CPU time). Perfetto is a stub with every tag
  off (`tracing_perfetto_stub.cc`, and `android.os.PerfettoTrace`'s natives,
  whose categories are never enabled); the `android.content.res` aconfig
  flag libandroidfw reads is fixed at its release value
  (`compat/framework/include/android_content_res.h`).
- **hwui stands in until views are drawn.** View and Drawable constructors
  call hwui (`RenderNode`, `Paint`, `Matrix`, `Path`); ColorSpace's named
  spaces are built by `DisplayManagerGlobal`. ColorSpace keeps its parameters
  (`android_runtime.cc`); the others' tables are generated from hwui's own JNI
  sources (`compat/framework/gen_hwui_standins.py`), so names and signatures
  match framework.jar. Every native answers through one System V function
  that takes no parameters (System V's caller pops the arguments): zero,
  false, null or 0.0 by return type; those that create an object return a
  zeroed block that their finalizer frees, and a few getters return a fresh
  object's non-zero defaults (scale and alpha 1, identity matrix). A class is
  added when the lifecycle reaches it.
- **libbinder without a driver.** Built without `__ANDROID__`, so
  `ProcessState`'s open of `/dev/binder` fails and only logs; local Binder
  objects, Parcel and in-process transactions work. The binder observer is off,
  as in the release. Its AIDL interfaces (IServiceManager and others) are
  generated at configure time by the aidl of Google's build-tools 36.1
  (downloaded with a checked hash, or `GESHER_AIDL`). frameworks/native patch
  0001: LLP64 size asserts, libbase's `Pipe`, `uid_t`. `binderthreadstate`'s
  header asks libhwbinder, which has a stand-in that serves no calls.
- **Tested at two levels**, in `tests/dex-natives`. Without framework.jar,
  stand-in framework classes with the same names and natives: the tables'
  natives, the `@CriticalNative` canary, the real Looper/Handler/MessageQueue
  sources, and libandroidfw on an aapt2-built APK (`tests/res-app`); stand-ins
  for classes with many natives get their declarations generated from the JNI
  tables (`tests/gen_jni_standins.py`). With framework.jar on the boot class
  path, the same library registers on the framework's classes
  (`FrameworkNativesTest`: MessageQueue, Parcel, Binder), and gesher-app runs
  the lifecycle.

- **gesher-app's own gaps** are its business ([app/README.md](../../app/README.md)):
  JCA providers that exist on the boot class path, with SHA-1, SHA-256 and
  MD5 digests until BouncyCastle or Conscrypt are boot jars (the framework
  digests partition fingerprints with SHA-1 from its first steps); the
  system features cache; placeholders for classes framework.jar references
  but lacks (`TelephonyServicesInitializer`).

## Consequences
- A registration added to the table needs its sources in `framework.cmake`,
  and, if its table uses `(void*)` casts, in `tools/jni-sysv-patch.py`.
- Natives no table registers fail with `UnsatisfiedLinkError` when called, as
  with libcore's ([0011](0011-libcore-natives.md)); `Process` grows as they
  appear.
- gesher-base gains `<syslog.h>` (to stderr), `<sys/cdefs.h>`, and
  `<linux/types.h>`, `<linux/ioctl.h>`, `<linux/sched.h>`, `<linux/memfd.h>`
  for libbinder's uapi headers.
