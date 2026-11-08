# Positioned I/O and real binary XML reload — 2026-11-08

## Observed Windows failure

The Windows 2022 and 2025 framework-contracts jobs for commit
`8af8d702bef18a7a6caead597595542b3aa41183`, run `42450333175`, reached the real
notification-channel persistence test. Reloading the files failed with
`Unexpected magic 1032ffff`; the subsequent group query returned null. The
Windows 2025 job is
[128639822766](https://github.com/gesher-runtime/gesher/actions/runs/42450333175/job/128639822766),
and the Windows 2022 job is
[128639822652](https://github.com/gesher-runtime/gesher/actions/runs/42450333175/job/128639822652).

Android 16's `Xml.resolvePullParser(FileInputStream)` sniffs four bytes using
`Os.pread` and then gives the same stream to the selected parser. The file really
starts with `ABX\0`. Gesher's common `pread` used `ReadFile` with an `OVERLAPPED`
offset on the original **synchronous** file handle. Microsoft documents that
this combination updates the file object's current position. The parser
therefore started four bytes late, at binary XML's document/tag/string tokens
`10 32 ff ff`. This was a native file-offset defect; notification policy storage
and the framework's parser selection were using their APIs correctly.

The production notification service and binary format stay unchanged.
`NotificationChannelGroupsTest.binaryPolicy` now explicitly checks the actual
binary header, unchanged sequential offsets zero and seven, and parsing through
`Xml.resolvePullParser` on a raw `AtomicFile.openRead()` stream. The existing
full restart and persisted group/channel assertions remain mandatory.

## Common native repair

`compat/posix/src/unistd.cc` now captures the actual file object under the
existing descriptor publication/replacement lock. `DuplicateHandle` pins that
object against concurrent close/reuse; the descriptor lock is released before
filesystem I/O. The original handle's granted data-access rights are checked
before requesting another handle, so a read-only descriptor cannot acquire
write access through reopening.

For regular files, `ReOpenFile` opens the same underlying file **by handle** with
an independent file object and `FILE_FLAG_OVERLAPPED`. No pathname is resolved,
and the original/duplicated descriptors' shared cursor is never saved, changed,
or restored. Existing write-through, no-buffering and sequential modes are
queried and retained. A private event and both file handles stay alive until an
`ERROR_IO_PENDING` operation completes. EOF returns zero; native failures remain
failures. Directories, pipes and virtual nonseekable descriptors receive the
corresponding errors without consuming or producing data. Null and zero-device
semantics are retained through their actual backends.

`pread64`/`pwrite64`, the bionic imports and fortified wrappers continue to route
through the same common functions. Successful writes notify inotify using the
pinned handle, avoiding accidental notification of a replacement file that has
reused the descriptor number. `internal.h` and `inotify.cc` add this narrow
handle overload; ordinary write behavior is otherwise unchanged.

## Executable evidence before Windows publication

| Check | Result and boundary |
| --- | --- |
| Production adapter over real Linux files | 74 assertions pass, including shared offsets, aliases, EOF, zero length, access failures, pipes/directories/devices, rename/unlink identity, pending completion, seven injected failure boundaries, no private handle leaks, close/reuse and concurrent ordinary reads. This is an explicit Win32 boundary model, not Windows execution. |
| Exact prior source negative control | The same 74 assertions against `8af8d7` fail 32 checks across all three test groups. The old synchronous-handle cursor behavior is reproduced at the boundary. |
| Independent Linux kernel reference | All 39 shared public-call assertions pass without the production adapter or Win32 model. |
| Windows compilation | Both complete changed implementation units, `unistd.cc` and `inotify.cc`, compile with `-Wall -Wextra -Werror` using the exact CI LLVM-MinGW 20261006 toolchain for both x86-64 and ARM64. |
| Actual framework test compilation | `PositionedIoTest.java` compiles against the verified Android core/framework classes. |
| Existing source preservation gate | `tools/net-tcp-priority/test-selected-policy.py` passes unchanged. The current selected TCP gate does not pin `unistd.cc`, `internal.h` or `inotify.cc`. No historical certificate or source predicate was relaxed. |

The host adapter ran with AddressSanitizer and UndefinedBehaviorSanitizer. Leak
Sanitizer cannot enumerate process threads in this container's PID namespace,
so its scan was disabled for that invocation; explicit handle-lifetime checks
still run on every success/failure path.

Reproduce the host checks from the repository root:

```sh
python3 tools/test_positioned_io_contracts.py --sanitize
python3 tools/test_positioned_io_contracts.py \
  --source-ref 8af8d702bef18a7a6caead597595542b3aa41183
cc -std=gnu11 -O1 -Wall -Wextra -Werror \
  tests/native/positioned_io_linux_reference.c -o /tmp/gesher-positioned-reference
/tmp/gesher-positioned-reference
```

The second command must fail. A writable `TMPDIR` may be selected when the
default temporary filesystem is full. On a container that blocks LeakSanitizer
thread enumeration, use `ASAN_OPTIONS=detect_leaks=0` for the first command and
retain that limitation in the result record.

## Required actual runtime checks and remaining limits

`PositionedIoTest` runs real Android `Os` calls through libcore JNI and the common
Windows adapter, with no native I/O mock. It exercises the binary sniff,
read/write permissions, high offsets, unchanged shared cursors, EOF, zero-length
operations, rename plus pathname replacement, an already unlinked file,
delete-on-close `memfd_create`, and concurrent ordinary/positioned I/O on
duplicated descriptors. The mandatory early service-track marker is
`PASS PositionedIoTest`. `positioned_io_contract_test.inc` also runs the public
imports and 64-bit aliases in the existing x86-64 and translated ARM64
`libc_test` suites.

**Windows execution of the new assertions is pending at this checkpoint.** In
particular, Linux's `/proc/self/fd` boundary model does not prove NTFS's reopening
behavior after unlink or with delete-on-close. These cases are required in the
actual runtime fixture and are not skipped or converted to expected success.

There is a real backend boundary for externally adopted handles whose sharing
mode forbids another open, or whose post-open permission/deletion state causes
Windows to reject `ReOpenFile`. Such refusal is propagated without moving the
original cursor; there is no unsafe seek/restore or pathname fallback. Complete
Linux semantics for every such handle are not established by this patch.
Gesher's ordinary file opens request read/write/delete sharing.

Linux also makes `pwrite` append when the open file description has `O_APPEND`.
Gesher currently retains that flag in the CRT descriptor rather than in the
native file object's data-access mode. This patch does not reconstruct that
status flag and does not claim to repair the pre-existing append behavior.
Append-only native access, when present, is not widened to arbitrary write
access by reopening.

## Primary contracts

- [Pinned Android 16 `Xml.resolvePullParser`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/util/Xml.java): positional sniff on `FileInputStream`, followed by the parser reading the original stream.
- [Microsoft `ReadFile`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile): a synchronous handle still updates its current position when an explicit `OVERLAPPED` offset is supplied; asynchronous completion and buffer lifetime requirements.
- [Microsoft `ReOpenFile`](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile): handle-based reopen, requested access, sharing restrictions and flags.
- [Microsoft explanation of duplicate handles and file objects](https://devblogs.microsoft.com/oldnewthing/20140711-00/?p=523): duplicates share the file object and its cursor; reopening creates a separate file object.
- [Microsoft `FILE_MODE_INFORMATION`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_mode_information) and [NtQueryInformationFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntqueryinformationfile): actual file modes and the mode query.
- [Linux man-pages `pread`/`pwrite`](https://man7.org/linux/man-pages/man2/pread.2.html): unchanged current offset, errors, and Linux's documented `O_APPEND` behavior.
