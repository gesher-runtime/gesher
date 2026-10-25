# 0045 — Other drives, the path calls, /proc, time zones, inotify and processes in gesher-base

**Status:** accepted, 2026-10-25 (posix track)

Extends [0010](0010-paths.md) (paths), [0009](0009-socket-fds.md) (fds that
gesher-base serves itself) and [0005](0005-posix-layer.md). Details and tests:
[posix.md](../posix.md).

## Context
0010 left four things open: paths on another drive cannot go in ART's
`:`-separated lists; `fopen`, `access`, `unlink`, `rename`, `chdir` and the
rest of the CRT's path calls take the ANSI code page and none of the Linux
names; `/proc` beyond `cmdline` is missing; symbolic links are not modelled.
Phase 1 also left the time zone data, inotify and a process model
(`getppid`, `kill` between processes) open.

## Decision

### Another drive is `/mnt/<drive>`
`/mnt/d/x` is `D:\x`, in every call that takes a path, and paths that leave
gesher-base (`getcwd`, `realpath`, `readlink` of `/proc/self/fd/<n>`) on a
drive other than the current one come out as `/mnt/<drive>/...`. A path on the
current drive keeps its drive-less form, as 0010 decided, and `/mnt/<current
drive>/x` names the same file as `/x`. So no Linux path holds a colon, and a
class path, library path or boot class path may name any drive.

`/mnt/<letter>` is the prefix 0010 reserved for this. WSL uses it too, so the
paths read naturally; a real `\mnt\<letter>` directory on the current drive is
shadowed, and `/mnt` itself and `/mnt/<longer name>` (Android's
`/mnt/sdcard`, ...) stay ordinary paths. The launchers (`gesher.ps1`,
`gesher-app.ps1`) write paths the same way, so a `.dex` or `.apk` may be on any
drive.

Rejected: a mount table (fstab-like, configured) — nothing needs more than
drives, and the mapping must be the same in every process without
configuration; `/c/...` (MSYS) — collides with one-letter directories at the
root, which Android's own paths do not use but apps may.

### Every path call goes through the path model
gesher-base defines `access`, `rename`, `remove`, `chdir`, `chmod`, `creat`,
`fopen`, `freopen` (and `fopen64`, `freopen64`) under the CRT's names, as it
already did `open`, `stat`, `mkdir`, `unlink`: in the UCRT import library each
is a member of its own. With `faccessat`, `renameat`/`renameat2`,
`unlinkat`, `fchmodat`, `fchdir`, `symlink`/`symlinkat`,
`readlink`/`readlinkat`, `link`/`linkat`, `mkdtemp`, `mkstemp`/`mkostemp`/
`mkstemps`, `nftw`/`ftw`, `fdopendir`/`dirfd`/`readdir_r`, all take UTF-8
Linux paths, `/dev` and `/proc` names, `/mnt/<drive>`, and paths longer than
`MAX_PATH` (made `\\?\` full paths).

- `rename` replaces the new name atomically, also while it is open
  (`FileRenameInfoEx` with POSIX semantics, `MoveFileEx` where the volume has
  none), replaces an empty directory with a directory, refuses a non-empty one
  (`ENOTEMPTY`), and does nothing for two names of one file; `RENAME_NOREPLACE`
  is supported, `RENAME_EXCHANGE` is not (`EINVAL`).
- `unlink`, `rmdir` and `remove` delete with POSIX semantics (the name goes at
  once, open or not), read-only files included.
- Permissions: the owner's write bit is `FILE_ATTRIBUTE_READONLY` (`chmod`,
  `fchmod`, `open(O_CREAT)`'s mode, `stat`, `access(W_OK)`); files have no
  execute bit in `stat`, directories do; `access(X_OK)` holds for every file
  (any file may be run, and apps ask `File.canExecute()` first).
- Symbolic links are NTFS symbolic links (and junctions, read only): `lstat`
  reports `S_IFLNK`, `readlink` gives the target as a Linux path (relative ones
  stay relative), `O_NOFOLLOW` refuses one with `ELOOP`, `unlink` removes a
  link to a directory. Creating one needs developer mode or the privilege
  (`EPERM` otherwise), as on Windows.
- `fopen` opens with gesher-base's `open` and `_fdopen`s the fd: Linux's
  sharing (an open file can be renamed and unlinked), always binary, glibc's
  `x` and `e` mode flags.

### /proc is generated
`/proc` names are virtual (procfs.cc): `open` reads a snapshot made at open,
`stat` reports size 0, `opendir` lists, `readlink` follows the links. Provided
for this process (`self` and its pid): `cmdline`, `comm`, `environ`,
`status`, `stat`, `statm`, `maps`, `mounts` (empty), `fd/` (links), `task/`
(`<tid>/comm`, `stat`, `status`), `exe` and `cwd` links; and `/proc/meminfo`,
`/proc/cpuinfo`, `/proc/uptime`, `/proc/mounts`. Other pids have no entries.

### Time zones: /etc/timezone and the tzdata module's files
libcore asks `RuntimeHooks`' supplier for the default zone, which only the
framework's `RuntimeInit` sets (from `persist.sys.timezone`), and otherwise
reads `/etc/timezone`, as on a Linux host. gesher-base serves `/etc/timezone`
as a virtual file holding Windows' zone as an IANA id: the Windows zone id
(`GetDynamicTimeZoneInformation`) and the user's region, through CLDR's
windowsZones table (generated at build time from the pinned ICU's
`windowsZones.txt`), as ICU's own Windows detection does. With automatic
daylight saving turned off in Windows, it is a fixed `GMT±hh:mm` zone.
Only `/etc/timezone` of `/etc` is virtual.

The data is the `com.android.tzdata` module's, from the pinned
`system/timezone` (`aosp.lock`), laid out as the module installs it under
`$ANDROID_TZDATA_ROOT/etc/tz` (tzdata, tzlookup, telephonylookup, and ICU's
`zoneinfo64`/`metaZones`/`timezoneTypes`/`windowsZones` overlay, format 9), in
`dist/tzdata` and installed with Gesher; the launchers point
`ANDROID_TZDATA_ROOT` at the install folder.

Rejected: a libcore patch calling a Windows-zone native from
`TimeZone.getDefaultRef` — the virtual file needs no Java change and gives
native code the same answer.

### inotify over ReadDirectoryChangesW
An inotify fd is a CRT fd on `NUL` mapped to its object, like eventfds and
epolls (0009); its readiness is a read-only eventfd, so `poll` and `epoll`
take it unchanged. Each watch is an overlapped `ReadDirectoryChangesExW` (a
watched file: its directory, filtered), on a completion port that a worker
thread turns into Linux's events and layout. Windows reports no close and no
open: `IN_CLOSE_WRITE` comes when a created or modified file is no longer open
for writing (checked every 50 ms by opening it with write sharing denied;
gesher-base's `open` waits out such a brief denial), `IN_OPEN`, `IN_ACCESS`
and `IN_CLOSE_NOWRITE` never come. libopenjdk's `LinuxWatchService.c`
(`java.nio.file.WatchService`) and libandroid_runtime's FileObserver natives
are built on it.

### Processes
- `getppid` is the creating process while it runs and is older than this one,
  else 1 (an orphan's parent on Linux).
- `kill` to another pid: every process linked with gesher-base keeps a
  mailbox (a named section `Local\gesher-signals-<pid>` and an event, watched
  by a thread-pool wait). The target delivers a signal as one sent to itself
  (signal.cc: pending until `sigwait`, as ART's signal catcher takes
  `SIGQUIT`/`SIGUSR1`), drops it if ignored, and for `SIGTERM`, `SIGINT`,
  `SIGHUP` with `SIG_DFL` ends with status 128 + signal. `SIGKILL` ends any
  process, `SIGSTOP`/`SIGCONT` suspend and resume it, signal 0 checks it; a
  process without a mailbox can only be ended (default-terminating signals).
  An exited process is `ESRCH`.
- `getuid` is `$GESHER_UID` when the starter of a process sets it, else 10000.
- Resource limits are kept per process at Linux's defaults (stack: the
  executable's reserve; core 0; files 8192, the CRT's limit and
  `sysconf(_SC_OPEN_MAX)`), lowered and raised within the hard limit.

## Consequences
- Every executable linked with gesher-base has the signal mailbox (a named
  section, an event and a thread-pool wait) from startup.
- `chdir` to `/mnt/<other drive>/...` changes what drive-less paths mean, as
  0010 noted for `chdir` to another drive.
- `signal()` stays the CRT's; only `sigaction` dispositions are seen by the
  mailbox (signal.cc's area).
