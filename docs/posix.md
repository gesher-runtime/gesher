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

Excluded by name (gtest filter in `21-posix.ps1`):

- `ArtDexFileLoaderTest.GetMultiDexChecksumsDexFile`: reads
  `art-gtest-jars-VerifierDeps.dex`, which is assembled from smali; the build
  has no smali assembler.

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
- `access(X_OK)` fails for every regular file, as `stat` reports no execute
  bit (`.exe` included).
- A change of Windows' zone while a process runs is not seen: libcore caches
  the default zone (Android resets it on `ACTION_TIMEZONE_CHANGED`).
- `app/build.prop` sets `persist.sys.timezone=UTC`; nothing reads it now (no
  `RuntimeInit`), but if the framework's time zone supplier is ever installed,
  that line would override Windows' zone.

## Status and next steps (2026-10-25, handoff checkpoint)

**CI-verified: nothing yet.** No run of this branch has been checked (GitHub's
API was rate-limited for all agents).

**Runs in flight (not watched, results unknown):** on commit 65cccdc (the first
push): ci run 40135642983 (full ci, workflow_dispatch) and posix run
40135639733 (posix.yml, push-triggered: the ART gtests on both runners). Both
predate the WIP commit after it, which adds LinuxWatchService, FileObserver,
their Java tests, a patch, this doc and ADR 0045 and fixes an argument-order
bug in inotify_init1 (`std::move(in)` next to `in->ready`).

**Verified locally only** (WinLibs gcc 16 UCRT on Windows 11, not the CI
toolchain; script outside the repo): gesher-base compiles with -Wall -Wextra
without warnings; `posix_probe` ("all passed", 3 runs), `socket_probe` and
`epoll_probe` pass; `posix_probe --timezone` printed Asia/Jerusalem for an
Israel-zone machine. The `/mnt/<drive>` branch of the probe was not exercised
locally (one drive). Nothing ART/libcore/Java was built locally.

**Next, in order:**
1. Read the two runs' logs (`gh run view <id> -R gesher-runtime/gesher
   --log-failed`, sparingly; `gh run watch <id> --exit-status --interval 120`).
   Expect clang `-Werror` fixes in compat/posix (gcc was used locally) and
   first-run problems in `cmake/aosp/art_gtests.cmake` (link set: it links
   art_runtime + art_compiler + disassembler stand-in like dalvikvm;
   common_art_test.cc may need more gesher-base calls).
2. Push the fixes; trigger `gh workflow run ci --ref wip/posix` once the
   posix.yml run builds; iterate until both runners pass every 21-posix.ps1
   line and every existing line. Add gtest exclusions to 21-posix.ps1 and the
   list above only with a reason.
3. Things to watch in the test job: the time zone loop (`Set-TimeZone`, needs
   admin; restores the zone in `finally`); TimeZoneTest needs the tzdata copied
   by run-tests.ps1 into `$root/tzdata`; FileObserverTest needs patch 0450 and
   the `android_util_FileObserver.cpp` source in framework.cmake;
   WatchServiceTest needs LinuxWatchService.c (removed from
   GESHER_OPENJDK_UNPORTED) and its symbols in compat/libcore/builtins.cc.
4. The package job checks `tzdata\etc\tz\versioned\9\tzdata`, `...\icu\zoneinfo64.res`
   and `licenses\TZDATA-NOTICES.txt` in the installed Gesher.
5. Then tick the Phase 1 roadmap boxes (time zone data; inotify part of
   "BoringSSL, epoll and inotify, a process model"; "Paths on another drive
   ... the rest of the CRT's path functions"; "ART's own gtests for
   libartbase/libdexfile") quoting the run id, and update this section.

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
