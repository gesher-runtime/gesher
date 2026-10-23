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
passed whole to `prctl`), system/core 0004 (tests, below), system/core 0005
(`FileMap::create` maps nothing for a zero-length region, as the mmap
branch does on Linux; `CreateFileMapping` fails on an empty file), frameworks/base
0004 (CursorWindow's Parcel code under `GESHER`; ZipUtils_test). Stand-in
`compat/framework/include/com_android_server_backup.h` (aconfig flag
`enable_max_size_writes_to_pipes`, ENABLED from bp1a on).

gesher-base additions (compat/posix): `<sys/select.h>` + `src/select.cc`
(select/pselect over poll), `<fnmatch.h>` + `src/fnmatch.cc`,
`<linux/capability.h>`, `random`/`srandom` (stdlib.h, unistd.cc), `bzero`
(string.h), `CPU_AND/OR/XOR/EQUAL` (sched.h), `NETLINK_KOBJECT_UEVENT`
(linux/netlink.h); from the tests' results: `posix_memalign` (`src/memalign.cc`:
over-allocates with malloc and records the aligned pointer; every module links
with `--wrap=free --wrap=realloc` so both accept it), IPv6 sockets dual-stack
by default like Linux (`IPV6_V6ONLY` 0; Windows defaults to 1), `readlink`
and `realpath` of `/proc/self/fd/<n>` (the file open on that fd), and
`readv`/`writev`/`preadv`/`pwritev` fail with EBADF on a bad fd even with no
buffers.

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
- libandroidfw `PosixUtils_test.cpp`: libandroidfw's `PosixUtils.cpp`
  compiles `android::util::ExecuteBinary` (fork/execvp) out on `_WIN32`, its
  one user (OverlayConfig's JNI running `/system/bin/idmap2`) is not built,
  and the test runs `/bin/date`.

Excluded by name in `20-libs.ps1` (gtest filter):

- libcutils `SocketsTest.android_get_control_socket`: binds an AF_UNIX
  socket under `/dev/socket`, the directory init creates for its daemons'
  sockets; gesher-base's path model has no `/dev/socket` (names under /dev
  never reach the disk, ADR 0010), and nothing in Gesher hands sockets over
  from init.

Test environment: `20-libs.ps1` sets `EXTERNAL_STORAGE` to a temporary
directory for libandroidfw_tests (ObbFile_test writes there; Android sets it
for every app). The tests build with Soong's global
`-ftrivial-auto-var-init=zero` (Unicode_test's zero-length cases read a
stack buffer the function under test leaves alone). Singleton_test's
`libutils_test_singleton2.so` names its exports in a `.def`: lld's
`--export-all-symbols` skips any symbol that also has an `__imp_` entry in a
linked import library, and singleton1's import library has all of them.

Tests AOSP itself compiles only for Android (in the sources): CallStack
`log_stack` (reads logd), Mutex `timedLock` (Android-only API).

Test source changes (system/core 0004, frameworks/base 0004), not exclusions:
Timers_test's death tests accept a nonzero exit status instead of SIGABRT
(Windows has no signal deaths); ashmem_base_test does not redefine
`getpagesize`; ZipUtils_test skips `tm_gmtoff`/`tm_zone` (absent from the
CRT's `struct tm`); CursorWindow_test's Parcel tests compile under `GESHER`.

## Status and next steps (2026-10-23, stopped at the account's usage limit)

**CI-verified (libs.yml run 39955519843, commit d8a54d8, both runners
green):** every suite passes on windows-2022 and windows-2025:

    PASS  libutils_binder_test: 113 tests passed
    PASS  libutils_binder_test_custom_allocator: 113 tests passed
    PASS  libutils_test: 79 tests passed
    PASS  libcutils_test: 85 tests passed (1 excluded, docs/core-libs.md)
    PASS  libandroidfw_tests: 291 tests passed

**Full ci (run 39953331538, commit 70a031d):** build, framework, libs build
and package green; windows-2025's test job green with every Phase 2 check
(probes incl. libutils probe, dalvikvm runtime/Hello/framework natives/
resources/MessageQueue/Parcel/Binder, dexdump, gesher-app lifecycle.apk to
onResume) and the five suites; on windows-2022 every Phase 2 check passed
too, and only libutils_test (Looper_test's delayed message, then
SystemClock_test) and libandroidfw_tests (IdmapTest.OverlayAssetsIsUpToDate)
failed. Those were fixed in gesher-base afterwards (0f68b1a: precise
poll/epoll deadlines, st_nlink on a pending delete; d8a54d8: unlink with
POSIX semantics, nanosleep64 on QueryPerformanceCounter) and verified by
libs run 39955519843 above, but **no full ci has run on d8a54d8 yet**.

**Next concrete steps:**
1. `gh workflow run ci -R gesher-runtime/gesher --ref wip/libs-full` on
   d8a54d8 (or later); check every Phase 2 PASS line and the five suites
   on both runners (the d8a54d8 gesher-base changes, `unlink` and
   `nanosleep`, touch every executable).
2. When that run is green: tick the roadmap box ("libutils, libcutils and
   libandroidfw in full") in docs/roadmap.md, quoting the run id here.
   docs/framework-natives.md's library section is already updated.

**Not done / known gaps:**
- ProcessCallStack: builds, but gesher-base has no `/proc/self/task`
  directory or `/proc/self/task/<tid>/comm`, so it lists no threads (no AOSP
  test covers it). Would need new PathKinds in compat/posix path.cc/dirent.cc.
- `systemTime(SYSTEM_TIME_MONOTONIC)` is still gettimeofday-based on Windows
  (Timers.cpp's host branch); a device-faithful version needs
  `CLOCK_BOOTTIME` and a monotonic `Condition` clock.

**Open problems / for the coordinator:**
- `compat/unwindstack/` and ART's `compat/art/unwindstack_stub.cc` both
  define `AndroidLocalUnwinder`'s virtuals; one executable must not link
  both (dalvikvm links only the stub today).
- libpng is added here as target `png`; the graphics line may need it too
  (aosp.lock conflict on `external/libpng`).
- libutils' `Mutex`/`Condition`/`Thread` change from AOSP's Windows variants
  to the pthread ones for every consumer (binder, androidfw, framework
  natives, hwui). libutils now defines `androidSetThreadName`, so the
  stand-in in `compat/framework/android_runtime.cc` was removed (graphics
  and services also edit that file; keep it removed when merging).
- gesher-base changes every track inherits: `--wrap=free --wrap=realloc` on
  every module (posix.cmake), AF_INET6 sockets dual-stack by default,
  precise poll/epoll deadlines, `nanosleep64`, `unlink`, `/proc/self/fd`
  links, `st_nlink` on a pending delete, readv/writev EBADF.
