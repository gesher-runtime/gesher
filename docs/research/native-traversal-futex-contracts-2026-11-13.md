# Android traversal and futex contracts

This note describes the native implementation rebuilt from `main` at
`b9a94530c6bf91f7a14913e3ef284dd76bdf28bf` on 2026-11-13. The preceding local
checkout became inaccessible before publication. The results below are fresh
runs of the rebuilt code; they do not reuse the earlier checkout's test counts.

## Traversal

The common `ftw`/`nftw` implementation now observes positive descriptor-budget
validation, callback return values, physical/logical traversal, ancestor-cycle
suppression, mount boundaries, directory-read failures, and physical `FTW_CHDIR`
callback directories. It enumerates one directory at a time, retaining child
names before descending, and restores the initial directory through an owned
file descriptor. An already completed directory reached through another alias
is visited again; only ancestor identities suppress a visit.

The behavior was checked against pinned Android 16 sources, rather than assuming
glibc's implementation has identical semantics:

- [Android `ftw.cpp`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/ftw.cpp),
  decoded SHA-256 `8ffd2e951f8f85d694eda7be831654d33e40cf00fb00819d3b273a867724e1ec`.
- [Android `fts.c`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/fts.c),
  decoded SHA-256 `4db7a5819c0024b73e4dd45a3b23c1f25a0dc854125b9b0cf24cfe118bdc02da`.

Relevant Android details include following the command-line symlink even with
`FTW_PHYS`; suppressing directory changes for logical traversal even when
`FTW_CHDIR` is requested; reporting a mount-boundary directory without descending
into it; retaining a failed-follow callback when `lstat` succeeds, including
`ELOOP`; translating that callback to `FTW_SLN` for `nftw` and `FTW_NS` for `ftw`;
and giving `/` a basename offset of zero. Android validates `nfds >= 1` but
otherwise does not use its value. Its implementation translates the four POSIX
flags and ignores other bits; GNU `FTW_ACTIONRETVAL` actions are not part of this
Android contract.

The bionic shim now registers `ftw` and `ftw64` alongside `nftw` and `nftw64`.
Callbacks retain nested invocation state. The ARM64 translator converts the
144-byte x86-64 stat callback payload into the 128-byte ARM64 structure, including
all timestamps, identity fields, block fields and zeroed padding.

Fresh validation:

- **447** shared traversal assertions pass with ASan/UBSan, including all sixteen
  POSIX flag combinations and an actual second filesystem.
- **34** extracted production adapter assertions pass with ASan/UBSan.
- The original baseline compiles and fails the behavioral regression fixture.
- Actual changed production units and the native fixture compile with the pinned
  LLVM-MinGW toolchain and `-Werror`.
- The guest fixture compiles with NDK r27c API26 for x86-64 and ARM64.

Run `python3 tools/test_traversal_contracts.py --sanitize` on Linux. The host
runner uses `/dev/shm` as the second filesystem by default; `--second-root` can
select another. Native and guest Windows fixtures require
`GESHER_TRAVERSAL_SECOND_ROOT` to identify an existing writable directory on a
*different* filesystem from their working directory. A missing or same-device
fixture is a failure, not an omitted mount test. Each fixture creates and removes
only its own temporary directories and links.

The implementation remains a path-based walk, not a complete replacement for
Android's FTS engine. Concurrent rename/replacement identity, process-wide cwd
interactions and every allocation-failure path are not qualified by these tests.

## Futex ownership and selection

The six implemented commands are `WAIT`, `WAKE`, `WAIT_BITSET`, `WAKE_BITSET`,
`REQUEUE`, and `CMP_REQUEUE`. Stack-resident intrusive waiters are indexed in 64
buckets under an SRW lock. Selection accounts for address, private/ordinary key
mode and bit mask. Counts report selected or moved waiters. A wake removes its
waiter and signals its state while holding the lock; the waiter takes the same
lock before returning, so no waker can retain a pointer to its expired stack.
Requeue changes queue membership without reporting a completed wait.

Deadlines use full-width arithmetic, validate nanoseconds before narrowing guest
arguments, and retain the original deadline across Windows wait slices or
spurious wakeups. Absolute realtime waits recheck the clock at intervals of at
most one second. This bounds response to clock changes; it is not a claim of
instantaneous Linux clock-change notification.

The queue owner is exported by the executable as `gesher_futex_owner_v1`.
Independently linked DLLs resolve that exact main-module export. An absent owner
returns `ENOSYS`; there is no local fallback that silently partitions waiters.

The linker must force the **private, unexported** symbol
`gesher_futex_owner_anchor_v1`. Forcing only the public owner name is insufficient:
with the DLL import library before the static archive, the linker can satisfy
that name from the DLL, leaving the executable without its required export.
The dedicated `futex_owner_link_test` calls only the DLL API and keeps this
import-library-before-static-archive order, so a direct executable syscall does
not mask the defect.

A fresh focused PE link comparison using the actual production syscall source
and pinned LLVM-MinGW compiler demonstrated both cases: the old option linked
successfully but imported the owner; the private anchor produced an executable
owner export without an owner import. The anchor was not exported. This focused
link probe supplied a stub for the unrelated `memfd_create` dependency; it is
compile/link evidence, not execution evidence or a complete product build.

Fresh validation:

| Gate | Result |
| --- | --- |
| ASan/UBSan public, mixed host/guest and concurrency groups | 396 assertions passed |
| ThreadSanitizer mixed host/guest and concurrency groups | 316 assertions passed, no race report |
| Same public fixture against the real Linux futex syscall | 232 assertions passed |
| Original production source, validation group | Compiled; failed 10 of 18 assertions |
| LLVM-MinGW production and native/DLL/bridge fixtures | Compiled with `-Werror` |
| NDK r27c API26 guest fixtures, x86-64 and ARM64 | Compiled with `-Werror` |

The queue admission handshake uses observable successful requeue, not a sleep
that guesses whether a new thread has reached its wait. Every ordinary fixture
wait has a deadline. The Windows bridge exercises native, bionic and separately
linked DLL wait/wake pairs, including direct guest timeout arguments with
nanoseconds equal to positive and negative 2^32.

Reproduce host gates:

```sh
python3 tools/test_futex_contracts.py --sanitize
python3 tools/test_futex_contracts.py --linux-reference
CXX='c++ -fsanitize=thread -fno-omit-frame-pointer' \
  python3 tools/test_futex_contracts.py --group guest --group concurrency
```

Local ASan/UBSan runs used `ASAN_OPTIONS=detect_leaks=0` because this execution
environment uses ptrace and LeakSanitizer refuses to run under it. The committed
runners do not disable leak checking in CI. Local ThreadSanitizer runs completed.

Priority-inheritance futexes, robust owner-death handling, `WAKE_OP`, Linux signal
interruption, cross-process shared futexes and shared-file aliases at different
addresses remain outside this engine. Ordinary and private modes are distinct,
but supported ordinary waits still require the same virtual address in one
process. These tests do not establish complete Linux or Android futex parity.

## Windows integration still required

The native executables are `traversal_test`, `futex_test`,
`futex_bridge_test`, and `futex_owner_link_test`. The latter two require the
separately built `futex_test_module.dll` beside them. The bridge links
`gesher_bionic` and `gesher_elf`; the owner-link executable must list the module
before `gesher_posix`. Both guest ABIs compile `traversal_contract.c` and
`futex_contract.c` and run through their real ELF loaders.

Fresh host and cross-compilation results do not establish successful execution
on Windows. The final PR's mandatory Windows gates must verify the real Win32
transport, separate PE ownership, filesystem behavior and translated guest
callbacks on the published head.
