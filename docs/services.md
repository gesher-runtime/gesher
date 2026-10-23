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
- **Locally** (WinLibs g++, Windows 11; not the CI toolchain): `binder_probe`
  passes end to end against `binderd` — context manager, a call with a Binder
  object, the nested call back into the waiting client thread, the server's
  object and the client's own object in the reply (the latter arriving as the
  client's pointer), `BR_INCREFS`/`BR_ACQUIRE` to the owner, death
  notification after the server exits, `BR_DEAD_REPLY` afterwards,
  `BC_CLEAR_DEATH_NOTIFICATION`. Local harness: scratchpad shims for
  open/ioctl/mmap (not committed).
- **CI**: run 39941566620 (commit "Binder across processes: binderd, ...")
  — see the result below.

### In progress (committed, not yet CI-verified)
- Commit "Java Binder across dalvikvm processes ...": the Java test
  (`BinderTest service|client` started by 50-services.ps1 with binderd and
  servicemanager), the frameworks/native patch 0002, the
  `android_runtime.cc` edits (thread attach + ServiceManagerNative) and
  framework.yml building `services-tests.dex`. Run CI on it first
  (`gh workflow run ci --ref wip/services`).

### Next steps
1. Get the three 50-services checks green (binder_probe, libbinder_probe,
   BinderTest). Likely trouble spots: clang -Werror in the new files; the
   libbinder_probe's AIDL usage (`checkService2`/`Service` union); the Java
   client's `ServiceManagerProxy` → `getNativeServiceManager` →
   `BackendUnifiedServiceManager` path (patch 0002); thread attach for
   `BR_SPAWN_LOOPER` threads (`_beginthreadex` threads attaching to ART).
2. Then tick the roadmap's "Binder across processes" box (only after CI).
3. System services (roadmap item 2), incremental, each verified:
   a. Real APK parsing: replace `app/java/com/gesher/app/AppManifest.java`
      with `com.android.internal.pm.parsing.PackageParser2` /
      `com.android.internal.pm.pkg.parsing.ParsingPackageUtils` (in
      framework.jar) and `PackageInfoCommonUtils`/`AppInfoUtils` to build
      PackageInfo/ApplicationInfo/ActivityInfo; keep lifecycle.apk's onResume
      check green.
   b. A system_server-lite process (Java, in gesher-app.jar, e.g.
      `com.gesher.server.SystemServer`) registering `package`, `activity`,
      `activity_task`, `window`, `display`, `user` with servicemanager over
      binderd, moving the `StandIn*` implementations there.
   c. The app process via `ActivityThread.main`-like attach:
      `IActivityManager.attachApplication` → `bindApplication` → the server
      schedules `ClientTransaction` (`LaunchActivityItem`,
      `ResumeActivityItem`) on `IApplicationThread.scheduleTransaction`, so the
      real `handleLaunchActivity` path runs. Real AMS/ATMS (services.jar) are
      out of reach for now (they need most of system_server); the server side
      is Gesher's, the client side the framework's.
   d. A launcher (`gesher-app.cmd` multi-process mode) that starts binderd,
      servicemanager and the server.
4. `.apk` file association (roadmap item 3): `tools/package/gesher.iss`
   `[Registry]` entries under HKCU\Software\Classes (`.apk` →
   `Gesher.Apk`, open command `"{app}\gesher-app.cmd" "%1"`,
   `uninsdeletekey`), and in ci.yml's package job: check the keys after
   install, run the command line from the registry on lifecycle.apk and
   expect `gesher-app: done`, check the keys are gone after uninstall. Not
   started.

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
