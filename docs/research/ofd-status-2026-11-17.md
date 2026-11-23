# Open file descriptions and native process transport, 2026-11-17

## Contract and implementation

Android's file status is a property of an open file description. Duplicated
descriptors retain that description and its kernel file position; descriptor
inheritance remains a property of each descriptor. A successful process launch
must preserve the inherited descriptions rather than rediscovering unrelated
copies of their flags in the new process.

The executable now owns the descriptor table and descriptor transaction lock.
Linked DLLs resolve that owner, rather than creating separate registries.
Snapshots duplicate the exact native object under the transaction and retain
its description after descriptor close/reuse. A lazy kernel mapping holds
shared status when a description is exported to a real child process. The
versioned startup manifest carries file handles, mapping handles, descriptor
numbers and logical device kinds; aliases share one mapping. Descriptors above
the CRT startup block's size limit are adopted by the importer. Private
bookkeeping handles are not implicitly inherited by later launches.

Regular I/O pins the native file and status before releasing the descriptor
transaction. Append submits one native write with both OVERLAPPED offset words
set to all ones. It does not use a process-local seek/write lock. Positioned
writes reopen the pinned file object, retaining actual access and mode, so the
Linux `pwrite`/`pwritev` O_APPEND behavior does not alter the ordinary shared
cursor. File/pipe `writev` gathers multiple nonempty spans into one write;
single spans have no copy, vectors up to 4 KiB use stack storage, and larger
vectors allocate their capped transfer size. Cancellation releases both the
buffer and pinned description. Virtual event objects retain Linux's existing
per-element legacy write behavior.

The host and guest stdio adapters use the same descriptor I/O and stream
state. See [the stdio research note](stdio-ofd-2026-11-17.md). Shell redirections
now use the ordinary open path, and shell raw-handle reads/writes capture a
description and use the same policy. A shell-created file/pipe handle is
registered as a real descriptor for its lifetime, preserving shared status
across subsequent launches.

Procfs snapshot construction still needs temporary write access internally.
Before publishing the result, it now duplicates the native handle with only
the requested read or O_PATH rights and registers that actual mode. Failure
cleanup retires the ordinary descriptor and its OFD. A metadata-only read-only
flag would have left native writes and writable mappings possible.

## Honest capability boundaries

Buffered opens do not advertise O_DIRECT or timestamp suppression. Unsupported
changes fail without partially changing other status bits; imported native
direct/synchronous modes are discovered from their actual handle modes. Native
append-only handles cannot clear append. O_PATH ignores data-access options
and rejects data I/O and F_SETFL.

The launcher inspects the target PE's export table as data while pinning the
image against replacement. It never loads target code to detect the importer.
Only an image exporting its own `gesher_ofd_owner_v1` receives the private
transport. A forwarding export is insufficient.

An ordinary Windows executable receives native handles and a CRT descriptor
block. Logical zero/random devices, process-local virtual resources, emulated
append/nonblocking-pipe policy, or descriptors beyond that block's limit fail
with EOPNOTSUPP. They are not exported as successful NUL substitutes. A foreign
program does not implement later shared F_SETFL changes. Closed standard
handles remain closed for these programs; no importer-dependent placeholder
is supplied to them.

The former inline fork/vfork path was removed. Both APIs return ENOSYS; no
synthetic child, injected synchronization byte, discarded real byte, or
application-specific exception remains in that path. Supported exec/spawn,
system/popen and Java process launch remain separate real process operations.
The existing exec implementation still changes the Windows process identity;
this is documented in the exec decision, not presented as a native fork.

## Integration required before approval

PR68's pipe backend must route both descriptor I/O and
`ReadFileDescription`/`WriteFileDescription` through the same native endpoint:

- The snapshot must keep the exact endpoint alive across close/reuse, dup and
  exec/spawn, including real EOF/EPIPE after the last peer closes.
- O_NONBLOCK changes must be observed across aliases and real processes;
  empty reads and full writes need native atomic decisions. Peek-then-read
  alone races another reader and is not sufficient.
- Reads/writes must honor interruption/cancellation and return only bytes
  actually transferred. SIGPIPE/EPIPE and poll readiness must follow the
  underlying endpoint's real peer/buffer state.
- A complete gathered vector must reach one pipe operation, including the
  PIPE_BUF rule; no second loop may split it into independent writes.
- `/proc/self/fd` reopening of pipe endpoints still uses the older dup path.
  Linux permits a fresh description with independently requested access and
  status. An invented unsupported error would not repair that contract. This
  remains a blocker until the actual same-buffer reopen is implemented.

The subsequent [repair candidate](pr73-repair-candidate.md) supplies kernel
message-pipe admission and explicit native differential gates. It is an
intermediate candidate; the new Windows gates and independent reopening are
not claimed complete by portable execution.

The later status-ownership repair captures exact handles under publication
locks and performs native mode discovery/changes after releasing those locks.
Lazy capture/dup/export retain the real description without querying a blocked
pipe under the global table mutex. Its independent differential probe proves
unrelated-descriptor progress and safe close/reuse within a declared Linux
boundary. Synchronous Windows file-object serialization, independent procfs
pipe opens and bounded worker retirement remain unresolved native requirements;
see the repair candidate for the exact evidence and limits.

The existing non-file virtual-resource implementations retain their own status
logic; the transport explicitly refuses unsupported process export. This work
does not claim complete Linux socket/eventfd fcntl flag support. Null-path
freopen remains explicitly unsupported. These limitations must not be turned
into broader capability claims.

## Validation evidence and limits

The Linux host tools compile production bodies with declared native boundaries
and use real files, pipes, shared mappings and spawned children. They are not
evidence of a Windows ABI or kernel test:

- `tools/test_ofd_transfer_contracts.py`: production OFD table and spawn
  builder, real shared mappings and child process import, alias status in both
  directions, high descriptor adoption, cleanup failures, PE capability
  parsing, and rejection/native transport paths.
- `tools/test_file_io_contracts.py`: native regular-file and raw-snapshot I/O,
  exact object pinning through descriptor reuse, logical devices and status.
- `tools/test_positioned_io_contracts.py`: production reopen/positioned/seek
  path, actual file cursor witnesses, pending/failing native operations,
  concurrent ordinary I/O, and append cursor retention.
- `tools/test_host_event_loop_contracts.py --case vector`: production vector
  dispatch and existing virtual-object handling, including one gathered file
  or pipe write and positioned append.
- `tools/test_stdio_contracts.py` and
  `tools/test_freopen_mode_contracts.py`: stream lifetime/mode/buffer contracts,
  with documented Android/glibc differences checked separately.
- The public descriptor corpus also runs directly on Linux. The process
  corpus starts a real child and grandchild and races two independently opened
  append writers using scalar write, writev and pwritev records.

`ofd_spawn_test.exe` supplies a separate actual Windows gate: a plain UCRT
executable with no Gesher link receives only native handles; a Gesher-aware
child imports high aliases, shared flags, logical devices and closed standard
descriptors. A shell child checks raw output against inherited append status.
The existing native/Android ELF descriptor, stdio, exec and process-append
fixtures remain mandatory current-commit runtime gates. The ordinary Windows
child explicitly clears CMake's directory-wide gesher_posix link default.

## Primary sources

- [AOSP syscall definitions](https://github.com/aosp-mirror/platform_bionic/blob/android16-release/libc/SYSCALLS.TXT).
- [AOSP stdio implementation](https://github.com/aosp-mirror/platform_bionic/blob/android16-release/libc/stdio/stdio.cpp).
- [Linux fcntl implementation](https://github.com/torvalds/linux/blob/master/fs/fcntl.c).
- [Linux file I/O implementation](https://github.com/torvalds/linux/blob/master/fs/read_write.c).
- [Linux pipe implementation](https://github.com/torvalds/linux/blob/master/fs/pipe.c).
- [Microsoft WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile).
- [Microsoft ReOpenFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile).
- [Microsoft handle inheritance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/handle-inheritance).
- [Microsoft PE format and export tables](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
- [Microsoft STARTUPINFOW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow).
