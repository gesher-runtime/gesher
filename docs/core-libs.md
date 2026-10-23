# Core libraries: libutils, libcutils, libandroidfw in full

Roadmap item (Phase 2 checklist): "libutils, libcutils and libandroidfw in
full". Design: [ADR 0014](decisions/0014-libs-core-libraries-in-full.md).
Branch: `wip/libs-full`.

## What is built

Each library builds the source set its Android.bp builds for the device,
with the target names and include directories Phase 2 gave them (`cutils`,
`utils`, `utils_binder`, `androidfw`), so other lines of work link as before.

| Library | Sources (cmake) | Not the device's file, and why |
|---|---|---|
| libcutils (`cutils`, `cmake/aosp/system.cmake`) | common; linux (`canned_fs_config`, `fs_config`); not_windows (`fs`, `hashmap`, `multiuser`, `str_parms`); android (`android_reboot`, `klog`, `partition_utils`, `qtaguid`, `uevent`); libcutils_sockets' android set (`android_get_control_file`, all `*_unix.cpp`) | `ashmem-host.cpp` for `ashmem-dev.cpp` (ashmem driver ioctls, sealed memfds), `trace-host.cpp` for `trace-dev.cpp` (tracefs `trace_marker`; on Windows it would only log an error per process) |
| libutils_binder (`utils_binder`) | all 8 sources; `utils_binder_custom_allocator` for its test | — |
| libutils (`utils`) | common, `Trace.cpp` (android), `Looper.cpp` (linux) | — |
| libutilscallstack (`utilscallstack`, new) | `CallStack.cpp`, `ProcessCallStack.cpp` (Windows-disabled in Android.bp) | unwinds through `compat/unwindstack/android_unwinder.cc` (Windows x64 unwinder, COFF symbols) instead of libunwindstack |
| libandroidfw (`androidfw`, `cmake/aosp/framework.cmake`) | all sources incl. `CursorWindow` (over libbinder's Parcel), `Png*.cpp` (libpng), `BackupData`/`BackupHelpers` (android), PathUtils | built with `STATIC_ANDROIDFW_FOR_TOOLS` as before (only silences two log lines) |
| libpng (`png`, new, `cmake/aosp/third_party.cmake`) | host configuration + `intel/` | — |

Patches: system/core 0003 (libutils' pthread branches under `GESHER`:
`Mutex`, `Condition`, `RWLock`, `Thread`, `Threads.cpp`; thread name pointer
passed whole to `prctl`), system/core 0004 (tests, below), frameworks/base
0004 (CursorWindow's Parcel code under `GESHER`; ZipUtils_test). Stand-in
`compat/framework/include/com_android_server_backup.h` (aconfig flag
`enable_max_size_writes_to_pipes`, ENABLED from bp1a on).

gesher-base additions (compat/posix): `<sys/select.h>` + `src/select.cc`
(select/pselect over poll), `<fnmatch.h>` + `src/fnmatch.cc`,
`<linux/capability.h>`, `random`/`srandom` (stdlib.h, unistd.cc), `bzero`
(string.h), `CPU_AND/OR/XOR/EQUAL` (sched.h), `NETLINK_KOBJECT_UEVENT`
(linux/netlink.h).

## Tests

`cmake/aosp/googletest.cmake` (gtest, gtest_main, gmock) and
`cmake/aosp/core_libs_tests.cmake` (target `core_libs_tests`, output
`out/core-libs-tests/`, not in the default build). `.github/workflows/libs.yml`
builds them (and libandroidfw's sparse test apps with
`tests/build-androidfw-test-apps.sh`, aapt2 against framework-res.apk),
uploads `libs-tests`; ci.yml's test job downloads it into `dist/libs-tests`
and `tests/windows/tracks/20-libs.ps1` runs each suite (timeout, gtest
filter for exclusions, PASS/FAIL lines). libs.yml also runs on pushes to
`wip/libs-full` and then runs the tests itself on windows-2022/2025.

Suites: libutils_binder_test, libutils_binder_test_custom_allocator,
libutils_test (+ the `libutils_test_singleton1/2.so` DLLs Singleton_test
loads), libcutils_test, libandroidfw_tests. Every source the device's test
module lists is built, except:

- libcutils `sched_policy_test.cpp`: tests libprocessgroup's cgroup
  scheduling policies (libprocessgroup is not part of this item; needs
  Linux cgroups).
- libcutils `ashmem_test.cpp`: tests the device's ashmem-dev.cpp through the
  ashmem driver's ioctls (`ASHMEM_SET_NAME`, ...) and `fork()`; libcutils on
  Windows uses ashmem-host.cpp (ashmem_base_test.cpp, which runs, covers it).

Tests AOSP itself compiles only for Android (in the sources): CallStack
`log_stack` (reads logd), Mutex `timedLock` (Android-only API).

Test source changes (system/core 0004, frameworks/base 0004), not exclusions:
Timers_test's death tests accept a nonzero exit status instead of SIGABRT
(Windows has no signal deaths); ashmem_base_test does not redefine
`getpagesize`; ZipUtils_test skips `tm_gmtoff`/`tm_zone` (absent from the
CRT's `struct tm`); CursorWindow_test's Parcel tests compile under `GESHER`.

## Status and next steps (2026-10-23, session wound down mid-way)

**CI-verified:** nothing of this item is verified yet; the roadmap box stays
unticked.

**Builds** (libs.yml push runs): run 39941775326 compiled every library
source above (libcutils, libutils, libutils_binder, libutilscallstack,
libandroidfw full, libpng, gtest/gmock; not yet the unwinder, item 1). Remaining
errors in that run:
1. `compat/unwindstack/android_unwinder.cc`: `ERROR_INVALID_PARAMETER` is a
   `<windows.h>` macro — fixed in the last commit (kErrorInvalidParameter).
2. `trace-dev_test.cpp`: `PROP_NAME_MAX` undefined off bionic — fixed in the
   last commit (test-only define, 32).
3. `posix_memalign` undefined when linking libutils_binder_test(s) and
   libandroidfw_tests: gesher-base's stdlib.h declares it but nothing defines
   it. Needs a gesher-base implementation whose memory `free()` accepts
   (`_aligned_malloc` memory cannot go to `free`): e.g. over-allocate with
   malloc only for alignment <= 16 (malloc's guarantee) and fail with EINVAL/
   ENOMEM otherwise, or find the caller (likely gmock or libbase) and see what
   alignment it asks for.
4. `android::util::ExecuteBinary` undefined (libandroidfw_tests): libandroidfw
   `PosixUtils.cpp` compiles it out under `_WIN32`, but the android-only
   `tests/PosixUtils_test.cpp` calls it. Either implement it for Gesher (a
   frameworks/base patch: CreateProcess-based, or posix_spawn if gesher-base
   gains it) or leave PosixUtils_test.cpp out with that reason.

Run 39942371602 (push of the last commit) was started to check fixes 1–2;
its result is not recorded here.

**Not yet run on Windows:** no suite has run yet, so pass/fail counts and
any further exclusions are unknown. Expected trouble spots: Singleton_test
(DLLs named `.so`, data auto-import of `Singleton<T>::sInstance` from
singleton1 into singleton2), CallStack_test (needs the unstripped test exe's
COFF symbols; the test exes are linked without `-s` for that), death tests
(gtest re-runs the exe on Windows), sockets_test (IPv6 loopback, the new
select), Looper_test, android_get_control_*_test (`/proc/self/fd` readlink),
fs_config_test, libandroidfw_tests' data paths (`GetExecutableDirectory()`).

**Not done:**
- Full ci run (Phase 2 checks: probes incl. libutils probe, framework
  natives, resources, gesher-app to onResume) on this branch with the new
  libutils threading (patch 0003) and the full libandroidfw. ci run
  39940802976 failed early on a typo since fixed; its framework host-tools
  job was rebuilding host tools (the aosp.lock googletest line changed:
  `googletest/ googlemock/`, which is part of framework.yml's host-tools key).
- ProcessCallStack: builds, but gesher-base has no `/proc/self/task`
  directory or `/proc/self/task/<tid>/comm`, so it lists no threads (no AOSP
  test covers it). Would need new PathKinds in compat/posix path.cc/dirent.cc.
- docs/framework-natives.md's library section still describes Phase 2's
  subset; update it when CI is green, then tick the roadmap box.

**Next concrete steps:** fix 3 and 4 above, push (libs.yml runs build and
the Windows tests), read the FAIL lines of 20-libs.ps1, fix in gesher-base
or list each remaining exclusion here with its reason; then
`gh workflow run ci --ref wip/libs-full` and check every Phase 2 PASS line
plus the five core-libs suites; then update framework-natives.md and the
roadmap.

**Open problems / for the coordinator:**
- `compat/unwindstack/` and ART's `compat/art/unwindstack_stub.cc` both
  define `AndroidLocalUnwinder`'s virtuals; one executable must not link
  both (dalvikvm links only the stub today).
- libpng is added here as target `png`; the graphics line may need it too.
- libutils' `Mutex`/`Condition`/`Thread` change from AOSP's Windows variants
  to the pthread ones for every consumer (binder, androidfw, framework
  natives, hwui); `systemTime(SYSTEM_TIME_MONOTONIC)` is still
  gettimeofday-based on Windows (Timers.cpp's host branch; a device-faithful
  version needs `CLOCK_BOOTTIME` and a monotonic `Condition` clock in
  gesher-base/winpthreads).
