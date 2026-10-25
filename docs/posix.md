# gesher-base's POSIX layer: paths, /proc, time zones, inotify, processes, ART's gtests

Phase 1 items closed by the posix track (branch `wip/posix`). Design:
[ADR 0045](decisions/0045-posix-paths-tz-procfs.md), on top of
[0005](decisions/0005-posix-layer.md), [0008](decisions/0008-linux-abi.md),
[0009](decisions/0009-socket-fds.md) and [0010](decisions/0010-paths.md).

## What is there

| Area | Where | What |
|---|---|---|
| Other drives | `compat/posix/src/path.cc` | `/mnt/<drive>/...` is `<DRIVE>:\...`; `getcwd`, `realpath`, `readlink` give it back for a drive other than the current one, so no Linux path has a colon |
| Path calls | `src/fsops.cc`, `src/links.cc`, `src/path.cc`, `src/stat.cc`, `src/dirent.cc`, `src/ftw.cc` | `access`/`faccessat`, `rename`/`renameat`/`renameat2` (atomic replace, POSIX semantics), `remove`, `unlink`/`unlinkat`/`rmdir` (POSIX delete, read-only files too), `chdir`/`fchdir`, `chmod`/`fchmod`/`fchmodat` (owner write bit = read-only attribute), `creat`, `open`'s mode and `O_NOFOLLOW`, `fopen`/`freopen` (over `open`, binary, `x`/`e`), `symlink`/`readlink`/`link` and their `*at` forms, `lstat` and `fstatat(AT_SYMLINK_NOFOLLOW)` of symbolic links, `mkdtemp`/`mkstemp`/`mkostemp`/`mkstemps`, `nftw`/`ftw`, `fdopendir`/`dirfd`/`readdir_r`/`telldir`/`seekdir`; UTF-8 names and paths past `MAX_PATH` everywhere |
| /proc | `src/procfs.cc` | `/proc/self` (and `/proc/<own pid>`): `cmdline comm environ status stat statm maps mounts`, `fd/<n>` links, `task/<tid>/{comm,stat,status}`, `exe`, `cwd`; `/proc/meminfo cpuinfo uptime mounts` |
| Time zones | `src/windows_zones.cc`, `gen_windows_zones.py`, `compat/libcore/collect-tzdata.sh` | `/etc/timezone` = Windows' zone as an IANA id (CLDR windowsZones from the pinned ICU, by the user's region), which libcore's `TimeZone.getDefault()` reads; the tzdata module's files from `system/timezone` in `dist/tzdata` and the installer (`ANDROID_TZDATA_ROOT`) |
| inotify | `src/inotify.cc`, `<sys/inotify.h>` | over `ReadDirectoryChangesExW`, an fd that `poll`/`epoll` wait on; libopenjdk's `LinuxWatchService.c` (`WatchService`) and libandroid_runtime's FileObserver natives are linked in |
| Processes | `src/process.cc`, `src/misc.cc` | `getppid`; `kill` to other processes (signal mailbox), `SIGKILL`/`SIGSTOP`/`SIGCONT` to any; `$GESHER_UID`; `getrlimit`/`setrlimit`/`prlimit`/`getrusage`; `prctl(PR_SET_NAME)` names the Windows thread too |
| libcap | `src/libcap.cc` | `cap_get_proc`, `cap_set_proc`, ... with empty sets (ART's tests use them) |

## Tests

- `probes/posix/posix_probe.cc` (`posix_probe.exe`, in the build job's
  artifact): paths (Hebrew names, a 12-level path past `MAX_PATH`, rename over
  an open file, directory renames, `RENAME_NOREPLACE`, chmod/read-only,
  fopen modes, hard and symbolic links, mkdtemp, chdir/fchdir), `/mnt/<drive>`
  on TEMP's drive when it is another one (on GitHub's runners it is),
  `/proc/self`, `/etc/timezone`, inotify (create, write, close-write, moves
  with one cookie, attrib, delete, directories, a watched file, oneshot,
  rm_watch, the watched directory deleted, in an epoll, a blocking read), and
  processes (getppid, `GESHER_UID`, SIGUSR1 to a child in `sigwait`, SIGTERM
  default and ignored, SIGSTOP/SIGCONT, SIGKILL, ESRCH), rlimits, libcap.
- `tests/dex-libcore/TimeZoneTest.java`: the default zone, the tzdata
  module's data loaded, and `java.util.TimeZone`, `java.time` and
  `android.icu` agreeing; `21-posix.ps1` sets Windows' zone to
  Eastern, Israel, AUS Eastern and UTC in turn and compares the offsets with
  .NET's (Windows' own rules) at every transition of 2025-2027.
- `tests/dex-libcore/WatchServiceTest.java`, `tests/dex-posix/FileObserverTest.java`
  (on framework.jar).
- A Hello whose class path entry is on another drive (`/mnt/c/...`).
- ART's own gtests, below.

All run from `tests/windows/tracks/21-posix.ps1` in ci.yml's test job on
windows-2022 and windows-2025.

## ART's gtests for libartbase and libdexfile

`cmake/aosp/art_gtests.cmake` builds `art_libartbase_tests` and
`art_libdexfile_tests` with every source their Android.bp lists, plus
`common_art_test.cc`, `testing.cc` and a main (`tests/art-gtests/art_gtest_main.cc`)
doing what `art/test/common/gtest_main.cc` does except gtest_extras'
`IsolateMain`, which forks a child per test: the tests run in one process.
`tests/art-gtests/build-test-jars.sh` builds the test jars they read
(`art-gtest-jars-{Main,Nested,Lookup,GetMethodSignature,MultiDex,MainEmptyUncompressed}`).
`.github/workflows/posix.yml` builds both into the `art-gtests` artifact
(on its own it also runs them on both runners); `21-posix.ps1` lays them out
as ART's host test setup expects (a `linux-x86` build tree, core-oj.jar under
`$ANDROID_ROOT/apex/com.android.art/javalib`, `$TMPDIR`).

`patches/art/0450-posix-gtests-on-windows.patch` makes two test sources
compile off Linux: `testing.cc` assigns `std::filesystem::path` to a
`std::string` (a `wstring` path on Windows: `.string()` added), and
`fd_file_test.cc`'s `Rename` test calls `CreateSparseSourceFile`, declared only
`#ifdef __linux__` (the guard also takes `GESHER`); `hex_dump_test.cc` expects
16-digit offsets with `__LP64__`, which ART takes 64-bit Windows to be (patch
`0002`) but the compiler does not define (`_WIN64` added).
`patches/art/0451-posix-fd-file-and-thread-stat.patch` changes libartbase
itself: `fd_file.cc` uses gesher-base's `pread`/`pwrite`/`fsync` (EOF reads 0,
a negative offset is `EINVAL`) and `fcntl(F_GETFD)` (`IsOpenFd`) instead of
ART's own `_WIN32` versions (which report EOF as `EIO` and every fd as open),
and `GetOsThreadStat`/`GetOsThreadStatQuick` read `/proc/<pid>/task/<tid>/stat` (gesher-base's
procfs) as on Linux. `art_gtest_common` also builds
`runtime/common_runtime_test.cc` (libart-runtime-gtest), the fixture of
`flags_test.cc`.

Not built: `base/aconfig_flags_test.cc`, which `static_assert`s the release
configuration's test flag values (`test`, `v_apis` true); Gesher's aconfig
values (`config/aconfig/art.values`) leave them false.

Excluded by name (gtest filter in `21-posix.ps1`):

- `FlagsTests*` (`flags_test.cc`): their fixture, `CommonRuntimeTest`, starts
  a runtime on the host build's whole boot class path (core-libart, conscrypt,
  okhttp, bouncycastle, apache-xml, ...), which the gtest layout does not
  have; the runtime aborts without it.
- `membarrier.private_expedited`, `membarrier.private_expedited_sync_core`:
  they expect a private barrier to fail with `EPERM` before registration;
  Gesher's `art::membarrier` (patch `0014`, FlushProcessWriteBuffers) treats
  registration as a no-op.
- `ScopedFlockTest.TestLocking`: ART's `LockedFile` returns "flock is
  unsupported on Windows" under `_WIN32`, and Gesher keeps that (it takes
  part in oat and profile file handling).

- `ArtDexFileLoaderTest.GetMultiDexChecksumsDexFile`: reads
  `art-gtest-jars-VerifierDeps.dex`, which is assembled from smali; the build
  has no smali assembler.
- `ArtDexFileLoaderTest.GetDexCanonicalLocation`: expects the location of a
  symbolic link to resolve to its target; ART's `DexFileLoader` skips
  `realpath` under `_WIN32` (`dex_file_loader.cc`), and Gesher keeps that, as
  changing dex locations would touch oat and boot image matching.

## Known gaps

- NTFS updates a file's directory entry, which `ReadDirectoryChangesW`
  reports, lazily: `IN_MODIFY` may come only when the writer closes the file,
  not at each write. `IN_OPEN`, `IN_ACCESS` and `IN_CLOSE_NOWRITE` never come;
  `IN_MOVE_SELF` only for a watched file; a watched directory moved elsewhere
  is not reported.
- `signal()` is still the CRT's; signal dispositions set with it are not seen
  by `kill` from another process (use `sigaction`).
- `/proc/<pid>` exists only for this process; `/proc/self/maps` has no
  `[anon:...]` names and its inode/device fields are 0.
- `stat` reports no execute bit for regular files, yet `access(X_OK)` (so
  `File.canExecute()`) holds for every existing file: Windows has no execute
  permission, and apps check `canExecute()` before they start their ELF
  executables through gesher-exec (docs/exec.md; Puzzles does).
- A change of Windows' zone while a process runs is not seen: libcore caches
  the default zone (Android resets it on `ACTION_TIMEZONE_CHANGED`).
- `app/build.prop` sets `persist.sys.timezone=UTC`; nothing reads it now (no
  `RuntimeInit`), but if the framework's time zone supplier is ever installed,
  that line would override Windows' zone.

## Status and next steps (2026-10-25, third agent, in progress)

**CI so far:**
- ci 40141424142 (at 47df386): every build job, package and e2e (except
  Puzzles, fixed since: `access(X_OK)` now holds for files) passed; the time
  zone loop broke on a PowerShell precedence bug (fixed).
- ci 40144053200 (at fc5b9b6): build, posix/build, package and e2e (all apps,
  Puzzles included) green; test jobs failed only on art_libartbase_tests
  (FdFileTest.PipeFlush hung: fsync of a pipe) and FileObserverTest (natives
  not registered: the test did not load libandroid_runtime). Passing there, on
  both runners: art_libdexfile_tests (119 tests, 2 excluded), posix probe, the
  class path on another drive, the four time zones, WatchService.
- ci 40191738786 (after merging integration 26aa8eb): cancelled; it hung in
  FileObserverTest (the observer thread kept the VM alive; the test now exits
  explicitly). libartbase showed Rename (directory fsync), HexDump (`_WIN64`),
  membarrier, ScopedFlock and FlagsTests failures: fixed or excluded (list
  above).

**Next:** watch the ci run of the latest push; fix what remains; then tick
the Phase 1 roadmap boxes with the run id and finish this section.

**Pitfalls met:**
- The Bash tool collapses `\\` to `\` inside commands and heredocs (a `'\\0'`
  became a NUL byte in path.cc once): write scripts with the Write tool.
- `GetFinalPathNameByHandleW` gives `\\?\C:\...`; made current as is, it hid
  the current drive (`_getdrive` 0). chdir/fchdir strip the prefix for short
  paths, and `CurrentDrive()` (path.cc) reads a `\\?\` cwd.
- NTFS reports writes to a directory watcher lazily (often at close).
- With GNU ld, gesher-base's weak `sigaction` was not pulled from the archive;
  the probe calls `gesher_sigaction_raw`.
- The `kill` checks start child copies of posix_probe; a hung run leaves
  `posix_probe.exe` processes that block relinking (kill them).

**Edits outside the track's own files (all additive or minimal):**
- `CMakeLists.txt`: `posix_probe` target; `include(cmake/aosp/art_gtests.cmake)`.
- `.github/workflows/ci.yml`: build job runs `compat/libcore/collect-tzdata.sh`;
  new `posix` job (posix.yml); test job `needs: posix` and downloads
  `art-gtests`; package job checks the tzdata files.
- `aosp.lock`: `system/timezone output_data/`; external/icu's sparse list gains
  `icu4c/source/data/misc/windowsZones.txt` (AOSP cache key changes).
- `tests/windows/run-tests.ps1`: copies `$Dir/tzdata` into `$root/tzdata`.
- `tests/build-test-apps.sh`: builds `posix-tests.dex` (tests/dex-posix with
  javac-only stubs in tests/dex-posix-stubs).
- `tools/package/gesher.iss` (tzdata + TZDATA-NOTICES.txt),
  `gesher.ps1` and `gesher-app.ps1` (`NoDrive` writes `/mnt/<drive>` for other
  drives; the same-drive checks for the .dex/.apk are gone;
  `ANDROID_TZDATA_ROOT` is the install folder's tzdata).
- `cmake/aosp/framework.cmake` + `compat/framework/android_runtime.cc`:
  `android_util_FileObserver.cpp` and its registration;
  `patches/frameworks/base/0450-posix-fileobserver-inotify.patch` (new).
- `patches/art/0450-posix-gtests-on-windows.patch` (new; test sources only).
- `cmake/aosp/libcore_native.cmake`: LinuxWatchService.c no longer unported
  (the exec track edits the same block for UNIXProcess_md.c).
- `compat/posix/src/signal.cc` (stack track's file): `kill` of another pid
  calls `gesher_kill_other` (process.cc) instead of failing with ENOSYS.
- `probes/posix/socket_probe.cc`: its two "/proc/self/maps and task are
  missing" checks now expect them to exist.

**For the coordinator:** process exit status for a signal death is 128 + signal
(TerminateProcess); the exec track's waitpid should decode it the same way.
`app/build.prop`'s `persist.sys.timezone=UTC` should go if RuntimeInit's time
zone supplier is ever installed.
