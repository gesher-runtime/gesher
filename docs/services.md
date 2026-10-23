# Binder and system services (Phase 4, services track)

Design: [0014](decisions/0014-services-binder-ipc.md). Tests:
`tests/windows/tracks/50-services.ps1` (dot-sourced by `run-tests.ps1`).

## Pieces

| Piece | Where |
|---|---|
| `/dev/binder` in gesher-base (client side of the driver) | `compat/posix/src/binder.cc`, `binder_internal.h`, `binder_wire.h`; hooks in `path.cc` (open, `PathKind::kBinder`), `fd.cc` (close/dup/dup2), `system.cc` (ioctl), `mman.cc` (mmap), `stat.cc`; `posix.cmake` adds bionic's uapi with `-idirafter` |
| binderd, the driver as a process | `compat/binder/binderd.cc` |
| servicemanager | frameworks/native `cmds/servicemanager` (Access.cpp, ServiceManager.cpp) + `compat/binder/servicemanager_main.cc`, `compat/binder/include/selinux/*` stubs; `cmake/aosp/servicemanager.cmake`; `aosp.lock` adds `cmds/servicemanager/` to frameworks/native |
| Probes | `probes/binder/binder_probe.cc` (raw ioctls: server = context manager, client, nested call, objects both ways, death), `probes/binder/libbinder_probe.cc` (libbinder + servicemanager across processes) |
| Java test | `tests/dex-services/gesher/services/BinderTest.java`, built by `tests/dex-services/build.sh` in framework.yml into `services-tests.dex` |
| libbinder → out-of-process servicemanager | `patches/frameworks/native/0002-servicemanager-over-binderd.patch` |
| Java threads for binder pools | `compat/framework/android_runtime.cc`: `androidSetCreateThreadFunc` (attach pooled threads to the VM, as AndroidRuntime's `javaCreateThreadEtc`); `android_os_ServiceManagerNative` registered (`framework.cmake` source added) |

Running by hand: `set GESHER_BINDER=mydev`, start `binderd.exe mydev`, then
`servicemanager.exe`, then any processes using binder (dalvikvm with
framework.jar, or the probes). `GESHER_BINDER_TRACE=1` makes binderd trace
every transaction on stderr.

## Status and next steps

### Done and verified
- **CI run 39954786992** (ci, both windows-2022 and windows-2025; commit 9ba0f5e):
  the branch builds again, every Phase 2 check still passes (gesher-app takes
  lifecycle.apk to onResume, installer job included), and the three checks of
  `50-services.ps1` pass:
  - `PASS  binder: /dev/binder across processes (binderd, raw ioctls)`
  - `PASS  binder: servicemanager and libbinder across processes`
  - `PASS  binder: two dalvikvm processes, ServiceManager, a Binder passed back, death`
  The first two already passed in run 39945861311 (which failed only on
  BinderTest, below).
- The dalvikvm link fix: libbinder's `UtilsHost.cpp` is built
  (`cmake/aosp/binder.cmake`), defining `execute`/`CommandResult` for
  `ServiceManagerHost.cpp` (pulled into dalvikvm by the registered
  `android_os_ServiceManagerNative`). `execute` is never reached (it serves
  the adb RPC service manager); fork fails with ENOSYS on gesher-base, so it
  would just return nothing. gesher-base's `sys/wait.h` gained
  `WIFCONTINUED`. This keeps the framework's own Java path
  (`ServiceManagerProxy` → native `getNativeServiceManager` →
  `BackendUnifiedServiceManager`, patch 0002) rather than overriding
  `ServiceManager.sServiceManager` from Java.
- BinderTest loads `$ANDROID_ROOT/build.prop` into the system properties
  first, as gesher-app does: the first `BinderProxy.transact` initializes
  `android.os.Build` (through AppOpsManager → ActivityThread), whose static
  initializer needs the ABI list (run 39945861311 died there with
  `ArrayIndexOutOfBoundsException` at Build.java:325 in both processes).
  `50-services.ps1` copies build.prop into `$root/system` itself.
- Local runs of `50-services.ps1` against CI's binaries (windows-x64 of run
  39945861311, framework artifact of run 39947844568) pass all three checks
  on Windows 11 too.

### Next steps
1. System services (roadmap item 2), incremental, each verified in CI:
   a. Real APK parsing: replace `app/java/com/gesher/app/AppManifest.java`'s
      reader with `com.android.internal.pm.parsing.PackageParser2`
      (`ParsingPackageUtils`, in framework.jar) and
      `PackageInfoCommonUtils.generate` for PackageInfo/ApplicationInfo/
      ActivityInfo; keep lifecycle.apk's onResume check green. A draft
      (untested, never built) is commit 7ac0162 on this branch, reverted in
      837cac7 when the scope of this relay was cut: a `PackageParser2.Callback`
      enforcing no compat change, the launcher activity from the parsed
      intent filters, `PackageInfoCommonUtils.generate` with every
      component, the old reader kept as a fallback that says so on stderr,
      and a `parsed by PackageParser2` log line a check can look for.
   b. A system_server-lite process (Java, in gesher-app.jar, e.g.
      `com.gesher.server.SystemServer`) registering `package`, `activity`,
      `activity_task`, `window`, `display`, `user` with servicemanager over
      binderd, moving the `StandIn*` implementations there. The stand-ins
      are `IFoo.Default` subclasses behind a `java.lang.reflect.Proxy`; to
      serve them across processes they need a concrete `IFoo.Stub` (abstract,
      so it can't be proxied): e.g. a build-time generator in app/build.sh
      that reflects over the AIDL interfaces in framework.jar's classes and
      writes `class FooStub extends IFoo.Stub` delegating every method.
   c. The app process via `ActivityThread.attach(false, startSeq)`:
      `IActivityManager.attachApplication` → `bindApplication` → the server
      schedules `ClientTransaction` (`LaunchActivityItem`,
      `ResumeActivityItem`) on `IApplicationThread.scheduleTransaction`, so the
      real `handleBindApplication`/`handleLaunchActivity` path runs. Expect
      trouble in the real `handleBindApplication` (LoadedApk's class loader
      goes through `ClassLoaderFactory`'s native
      `createClassloaderNamespace`, not registered; hwui and ddm setup).
      Real AMS/ATMS (services.jar) are out of reach for now; the server side
      is Gesher's, the client side the framework's.
   d. A launcher (`gesher-app.cmd` multi-process mode) that starts binderd,
      servicemanager and the server.
2. `.apk` file association (roadmap item 3): moved to a separate branch by
   the coordinator. A draft is commit 25bc4b1 on this branch (reverted in
   837cac7): `[Registry]` in `tools/package/gesher.iss` (`.apk` →
   `Gesher.Apk` and `OpenWithProgids` under HKCU\Software\Classes, open
   command `"{app}\gesher-app.cmd" "%1"`, `ChangesAssociations=yes`) and
   package-job checks in ci.yml; never run in CI.

### Open problems, dead ends ruled out
- `access("/dev/binder")` uses the CRT's `_access` (gesher-base has no
  `access` override), so libbinder's `selfIfKernelBinderEnabled` cannot see
  the device; patch 0002 avoids it rather than changing gesher-base's
  `access` (another track's area; roadmap Phase 1 item).
- servicemanager's Looper main (binder fd polling + timerfd) is not used:
  binderd has no poll support. Adding poll would need an asynchronous
  "work available" notification per thread connection.
- `BINDER_TYPE_FD` is not supported yet; needed once services pass
  ParcelFileDescriptors/SharedMemory (e.g. bindApplication's font map —
  the server can pass null).
- All processes report uid 10000 (gesher-base's getuid). Access off-Android
  reports uid 0 to ServiceManager, so registrations are allowed; system
  services checking `Binder.getCallingUid()` will see 10000 for everyone.
- Local builds: WinLibs g++ warns (`-Warray-bounds` false positives in
  vector inserts) where clang does not; CI uses llvm-mingw.
