# 0010 — Linux paths on Windows

**Status:** accepted, 2026-10-21

## Context
AOSP code writes Linux paths. Most name ordinary files, but some name
Linux's virtual file systems: libcore's `FileDispatcherImpl_preClose0` and
`io_util_md.c` open `/dev/null` and `dup2` it over the fd being closed, and
ART reads `/proc/self/cmdline` and `/proc/self/maps`. Passed to the CRT,
`/dev/null` means `\dev\null` on the current drive, and fails with `ENOENT`.

ART also joins paths into `:`-separated lists (`-Xbootclasspath`, `-cp`,
`java.library.path`, class loader contexts), and Java splits
`java.class.path` on `:` too. A drive letter's colon breaks these lists.
`tests/windows/run-tests.ps1` already gives ART drive-less paths for this
reason, and the Android root, ART root and data directories (patch 0008)
come from the environment as such paths.

## Decision
- **A Linux absolute path is a path on the current drive.** `/a/b` is
  `\a\b`, which is how Windows already reads a rooted path without a drive.
  So ordinary paths need no translation, every function agrees on them,
  gesher-base's and the CRT's alike, and lists of them contain no drive
  colons. `/` is not the Android root: there is no chroot or mount table,
  and `ANDROID_ROOT`, `ANDROID_ART_ROOT` and `ANDROID_DATA` say where
  Android's directories are (patch 0008).
- **Windows paths are accepted as they are**: `C:\x`, `C:/x`,
  `\\server\share`, `\\?\...`. A leading `//` is a UNC path, which POSIX
  allows (it leaves a leading `//` to the implementation). Relative paths
  are relative to the current directory.
- **Paths that come out of gesher-base are in the same form**: `getcwd` and
  `realpath` use `/` separators and drop the drive when it is the current
  one, so a path from them can go in a `:`-separated list and Java's
  `UnixFileSystem` sees it as absolute.
- **`/dev`, `/proc` and `/sys` are virtual: their names never reach the
  disk.** Supported names:

  | Name | Windows |
  |---|---|
  | `/dev/null` | `NUL` |
  | `/dev/zero` | an fd on `NUL` whose reads return zeros |
  | `/dev/random`, `/dev/urandom` | an fd on `NUL` whose reads come from `BCryptGenRandom` |
  | `/proc/self/cmdline` (and `/proc/<pid>/cmdline` for this process) | an anonymous file holding the arguments (`CommandLineToArgvW`), each followed by a NUL |

  Any other name under them fails with `ENOENT` (`stat`, `open`,
  `opendir`), which is what ART's readers of `/proc` already handle: they
  log less, or fall back.
- **Paths longer than `MAX_PATH`** are made full and given the `\\?\`
  prefix, so they work without a long-path manifest.
- **Where paths enter gesher-base** (`src/path.cc`, `gesher::MapPath`):
  `open`, `openat`, `stat`, `lstat`, `fstatat`, `utimensat`, `utimes`, `mkdir`,
  `mkdirat`, `rmdir`,
  `opendir`, `realpath`, and `AF_UNIX` names in `bind`, `connect` and
  `sendto`. AOSP code calls `open`, `stat`, `mkdir` and `getcwd` in the CRT
  directly, so gesher-base defines them under the CRT's names, as it does
  `close` and `read` ([0009](0009-socket-fds.md)): in the UCRT import
  library each is a member of its own, which the linker no longer pulls in.
  `stat`, `fstat` and `lstat` (`src/stat.cc`) fill Linux's `struct stat`
  from Win32 file information: libcore reads `st_atim`/`st_mtim`/`st_ctim`
  with nanoseconds, `st_blksize` and `st_blocks`, which mingw's struct
  lacks, so gesher-base's `<sys/stat.h>` renames mingw's struct and
  functions out of the way and defines Linux's. `st_ino` is the file index
  and `st_dev` the volume serial number; `fstat` reports sockets as
  `S_IFSOCK` and pipes as `S_IFIFO`.
- **`open` takes Linux's `O_*` values** ([0008](0008-linux-abi.md)) and
  opens with `CreateFileW` rather than the CRT's `_open`. The CRT's flags
  have other meanings (Linux's `O_CREAT` is the CRT's `_O_TEMPORARY`, which
  deletes the file on close), and `CreateFileW` gives Linux behaviour the
  CRT lacks: files are opened sharing delete access, so an open file can be
  unlinked or renamed; directories open (for `openat`);
  `O_DIRECTORY` gives `ENOTDIR`, writing to a directory `EISDIR`;
  `O_DSYNC`/`O_SYNC` write through. `openat`, `fstatat` and `mkdirat` resolve
  a relative path against the directory fd's path.

Rejected:
- **A gesher root as `/`** (`/system`, `/data` under one directory, like a
  chroot): every host path, from the command line, the current directory or
  a file dialog, would need translating both ways and a mount table to reach
  the rest of the disk.
- **MSYS- or Cygwin-style drives** (`/c/Users`, `/cygdrive/c`): the prefix
  collides with real directories on the current drive, and every path
  leaving gesher-base would need it added.
- **Teaching ART's `Split(…, ':')` to skip `X:`**: dozens of call sites in
  ART, plus Java code (`java.class.path`, `File.pathSeparator` users) that
  gesher cannot reach with a C++ patch.

`probes/posix/socket_probe.cc` tests the devices, `/proc/self/cmdline`,
open flags, the `*at` calls, drive-less paths, `AF_UNIX` names and the
`preClose0` pattern (`dup2` of a `/dev/null` fd over a socket that another
thread is blocked in `recv` on).

## Known gaps
- Everything ART lists must be on one drive, the current one. A path on
  another drive works where a single path is expected, but cannot go in a
  `:`-separated list. If this is ever needed, a reserved prefix such as
  `/mnt/<drive>` can be added to the virtual names without changing
  anything else.
- Functions still passed to the CRT unchanged: `fopen`, `access`, `unlink`,
  `rename`, `chdir`, `_wopen` and the CRT's other wide or underscore
  forms, `utime`. Ordinary paths work there; `/dev` names and paths
  longer than `MAX_PATH` do not.
- `/proc/self/maps`, `/proc/self/task`, `/proc/self/fd`, `/proc/cpuinfo`,
  `/dev/tty` and the `readlink` targets `/proc/self/exe` and
  `/proc/self/fd/N` are not provided.
- `O_NOFOLLOW` and the mode argument are ignored (permissions are not
  modelled); `O_TMPFILE` fails with `EOPNOTSUPP`. A file created by `open`
  does not get Linux's permission bits.
- `pread` on `/dev/zero` or `/dev/random`, and `mmap` of `/dev/zero`, see
  the underlying `NUL`.
- The current drive is read on every call, so `chdir` to another drive
  changes what drive-less paths mean, as it does on Windows.
