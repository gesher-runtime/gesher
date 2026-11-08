# Actual `Os.memfd_create` routing and file ownership — 2026-11-08

## Observed Windows failure

Both focused jobs for published commit
`223d4b3b6a4947bd3c1b3c679d95fc944b4d956a`, run `42453120565`, reached
`PositionedIoTest.ordinary` line 90 and failed with `memfd_create failed:
ENOSYS`, through `libcore.io.Linux.memfd_create` and `android.system.Os`.
The jobs are [Windows 2022, 128649121906](https://github.com/gesher-runtime/gesher/actions/runs/42453120565/job/128649121906)
and [Windows 2025, 128649121888](https://github.com/gesher-runtime/gesher/actions/runs/42453120565/job/128649121888).
The earlier regular-file, access, shared-cursor, rename, unlink and pipe checks
had completed; the later memfd and concurrent-I/O checks did not run. In the
same jobs, the actual binary notification-policy reload passed, confirming
the positioned-read repair fixed the original ABX header/cursor defect.

Pinned Android 16 `Linux_memfd_create` calls
`syscall(__NR_memfd_create, name.c_str(), flags)`. Gesher already had a real
delete-on-close file backend, but the common dispatcher returned `ENOSYS` for
this number. The x86-64 Android guest syscall entry also lacked number 319.
The existing ARM64 translator already maps number 279 to the public export.

## Actual implementation

Both missing entries now delegate to the same backend. The original
`Os.memfd_create` regression is unchanged; no direct-call replacement bypasses
the framework route.

The common backend also had input and ownership defects. The correction
stays inside `memfd_create`:

- Unknown flags and conflicting `MFD_EXEC | MFD_NOEXEC_SEAL` return `EINVAL`
  before accessing the name. Huge-page size bits require `MFD_HUGETLB`.
- Empty names and up to 249 name bytes are accepted. Longer names return
  `EINVAL`; null, unreadable or overflowing addresses return `EFAULT`.
  Bounded `ReadProcessMemory` calls avoid unsafe native string dereferences
  and avoid reading beyond a terminating byte at a page boundary.
- Supported flags are zero, `MFD_CLOEXEC`, `MFD_EXEC`, and their combination.
  The default policy permits executable mappings. Actual Windows read,
  write and execute access is requested; native access failures propagate.
  The existing `mmap` implementation performs executable file mappings, and
  regressions require them for both default and explicit `MFD_EXEC`.
- `SECURITY_ATTRIBUTES.bInheritHandle` now matches `MFD_CLOEXEC`. Previously
  the null security argument made every handle noninheritable, even for
  flags zero. Tests observe actual public `fcntl(F_GETFD)` state.
- A `GetTempPathW` required size at or above the buffer bound returns
  `ENAMETOOLONG` before using the buffer. Failed `CreateFileW` explicitly
  calls `DeleteFileW` for the file already created by `GetTempFileNameW`, preserving the original error
  even if cleanup changes `GetLastError`. Failed descriptor publication
  still closes the delete-on-close handle.

Valid requests for `MFD_ALLOW_SEALING`, `MFD_HUGETLB` or `MFD_NOEXEC_SEAL`
return `EOPNOTSUPP` before creating a file. These remain explicit compatibility
gaps, not implemented features. No partial sealing model was introduced. The
existing advisory `fcntl` seal handling elsewhere has not become enforcement
of write, resize or execute restrictions.

Storage remains a real Windows temporary file with delete-on-close lifetime,
rather than Linux shmem. Host temporary-directory ACLs still apply. The patch
does not establish shmem accounting, all mount-policy independence,
diagnostic-name exposure, huge-page support, or enforced seals.

## Executable evidence

| Check | Result and boundary |
| --- | --- |
| Exact production backend and both dispatcher bodies, default GCC `c++`, ASan/UBSan | **193 assertions pass** across direct storage, common syscall, x86-64 guest syscall, and injected native failures. Real Linux files, descriptors and mappings back the Win32 boundary model. |
| Exact published `223d4b` negative control | **57 failures among 121 executed assertions**, across all four groups. Missing descriptor creation prevents downstream I/O checks from running; this is not reported as a 193-assertion execution. |
| Independent Linux reference | Ten actual Linux `memfd_create` checks pass for empty/249/250-byte names, null/inaccessible pointers, flag-before-name ordering, huge-size flag validation, conflicting execute policy, and default/CLOEXEC state. |
| Complete Windows source units | `system.cc`, `syscall.cc`, and `shim.cc` compile with strict warnings for x86-64 and native ARM64 using the exact CI LLVM-MinGW 20261006 toolchain. |
| Shared C fixture | The shared test body compiles as C11 with `-O2 -Wall -Wextra -Werror` on the Linux host. No Android NDK is installed locally; this is not Android ELF compilation. The existing native-library CI build must compile and run both Android ABIs. |

The host model checks native creation/open/publication failures, original
error preservation, no leaked private handles or created paths, descriptor
lifetime through duplication, actual storage/resizing, positioned cursors,
executable mappings, flag validation and protected name pages. Unknown
syscalls still return `ENOSYS`.

This container denies even `process_vm_readv` on itself. The host probe model
therefore reads actual `/proc/self/maps` permissions before copying controlled
fixture bytes; those mappings do not change concurrently. It does not prove
Windows `ReadProcessMemory`. The model also unlinks transferred temporary
files while their real Linux descriptors retain storage, so it does not prove
Windows deletion timing. LeakSanitizer's thread scan is unavailable in this
container; explicit file/handle ownership checks remain enabled.

```sh
python3 tools/test_memfd_contracts.py --sanitize
python3 tools/test_memfd_contracts.py \
  --source-ref 223d4b3b6a4947bd3c1b3c679d95fc944b4d956a
```

The second command must fail. If LeakSanitizer cannot enumerate threads, use
`ASAN_OPTIONS=detect_leaks=0` and record that limitation. A writable `TMPDIR`
may be selected independently.

The shared `memfd_contract_test.inc` is mandatory inside the x86-64 and
translated ARM64 `libc_test` suites. It calls the public export through `dlsym`
and the architecture's raw syscall, tests actual storage/mappings and final
close, and exercises errors. The unchanged framework `PositionedIoTest` still
requires its original memfd and concurrency checks. **Windows execution after
this routing repair is pending at this checkpoint.**

## Exact source preservation

The complete updated `system.cc` SHA-256 is
`e1368c02fcec5f3626722ef57b1f917b88d9c481cb29fbd8e20e4a3f6b6f3b5d`.
Only the function from `int memfd_create(` to the following `// --- strsignal,`
boundary changed in this follow-up. The prior file-allocation repair and all
unrelated bytes remain. The active selected-TCP gate separately pins the full
file and compares every byte outside those two reviewed functions. Historical
certificates and unrelated supplier predicates remain unchanged.

## Primary sources

- [Pinned Android 16 libcore Linux JNI](https://android.googlesource.com/platform/libcore/+/android-16.0.0_r4/luni/src/main/native/libcore_io_Linux.cpp), `Linux_memfd_create`, is the actual raw syscall caller. Downloaded SHA-256: `996b37f8b5388ba40ebbf992053ebb87a463b6e39a60ffb4132cfa0af0379fb9`.
- [Pinned Android 16 common-kernel `mm/memfd.c`](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r1/mm/memfd.c) defines flag/name validation, ordering and sealing. Downloaded SHA-256: `b1176143e46387da48ea209cfe6177927fb14b3b08d2a0a57fdf3cff7afce458`.
- [Microsoft `CreateFileW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew), [`GetTempFileNameW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettempfilenamew), and [`GetTempPathW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppathw) define actual inheritance, deletion, temporary-file creation and buffer sizes.
- [Microsoft `ReadProcessMemory`](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory) and [`CreateFileMappingW`](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw) define readable ranges and executable file-mapping access requirements.
