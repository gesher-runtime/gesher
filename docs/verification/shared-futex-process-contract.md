<!-- SPDX-License-Identifier: Apache-2.0 -->
# Cross-process futex contract

This fixture measures [N29 and T02](https://github.com/gesher-runtime/gesher/issues/56) using the public Linux futex syscall. Two independently launched processes map the same file. A release/acquire handshake establishes that the mapping really is shared. The waker then uses an address different from the waiter's address, tests a wrong bitset, byte offset and private key, and requires the matching wake to report exactly one waiter. The waiter must return from the syscall before its monotonic deadline.

The [Android common kernel key lookup](https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/kernel/futex/core.c) keys a file-backed shared futex by backing file, page index and byte offset. The [wake implementation](https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/kernel/futex/waitwake.c) also checks the bitset intersection.

## Build and run

Build one PIE per supported guest ABI with the Android NDK. Use `aarch64-linux-android35-clang` or `x86_64-linux-android35-clang` from the NDK compiler directory.

```sh
aarch64-linux-android35-clang -std=c11 -O2 -Wall -Wextra -Werror \
    tests/native/shared_futex_process_contract.c -o shared-futex
adb push shared-futex /data/local/tmp/shared-futex
adb shell 'chmod 755 /data/local/tmp/shared-futex && cd /data/local/tmp &&
    dir=$(mktemp -d ./shared-futex.XXXXXX) &&
    trap '\''rm -rf "$dir"'\'' EXIT &&
    ./shared-futex setup "$dir/word" || exit 2
    ./shared-futex waiter "$dir/word" & waiter=$!
    ./shared-futex waker "$dir/word"; waker_status=$?
    wait "$waiter"; waiter_status=$?
    [ "$waker_status" -eq 0 ] && [ "$waiter_status" -eq 0 ]'
```

On Windows, place the exact same ELF alongside or point it at a build of `gesher-exec.exe`. Use a new file on the runtime drive. Pass its absolute guest path (for example `/Temp/shared-futex.unique/word`, with its parent directory already created). Run the following in two terminals, using the same path and matching ABI in both. The first terminal blocks for up to seven seconds in `waiter`; start the second after `setup PASS` or while waiting for `waiter READY`.

```powershell
# First terminal
& $launcher --elf $elf shared-futex setup $guestPath
if ($LASTEXITCODE -ne 0) { throw 'setup failed' }
& $launcher --elf $elf shared-futex waiter $guestPath
# Second terminal
& $launcher --elf $elf shared-futex waker $guestPath
```

Keep both process exit codes and transcripts, the ELF digest, the Android fingerprint and ABI, and the Windows runtime build and revision. A successful row requires both participants to report `PASS` and exit zero. Exit 1 is a failed contract; exit 2 is a setup or rendezvous error. Delete the fresh Windows file after both processes have exited. The test does not exercise priority inheritance, robust-list owner death, or the pthread process-shared API.

## Recorded executions

NDK 28.2.13676358 produced the arm64 PIE SHA-256 `ac5855e857e4ce8a487df3819a85ebe012c9cd6c363e851d32aff1e45cda2365` and the x86-64 PIE SHA-256 `9fb2a560b5b0f23e8da3afa2e3b1963afde2fae4b994316e8ddc9710bee8bc4e`.

| Environment | Guest ABI | Observed result |
| --- | --- | --- |
| Android 16 API 36 emulator, `Android/sdk_phone64_arm64/emu64a:16/BE2A.250530.026.D1/13818094:userdebug/test-keys` | arm64 | Five matching pairs. With no waker, the waiter timed out and returned 1. |
| Android 14 API 34 emulator, `google/sdk_gphone64_arm64/emu64a:14/UE1A.230829.050/12077443:userdebug/dev-keys` | arm64 | Five matching pairs. With no waker, the waiter timed out and returned 1. |
| Gesher portable `0.2.0.3123` from [CI run 45668397258 at `156abd94`](https://github.com/gesher-runtime/gesher/actions/runs/45668397258), Windows 11 Pro ARM64 in Parallels running the x64 launcher | arm64 and x86-64 | Setup and shared-memory rendezvous succeeded. The waker reported zero matching cross-process wakes and exited 1. The waiter reported `ETIMEDOUT` and exited 1. |

The Windows launcher SHA-256 was `6c8f2215f78a46ecc115ade86bf8fa804d5b25e6f5af4bd3af6ea758a9f219d2`. The retained build establishes a Windows baseline for the exact test ELFs. The current source revision and an x86-64 Android reference still need runs.

## Windows component fixture

From the repository root, build the Windows fixture with the project-pinned
LLVM-MinGW 20261006 compiler. Repeat with the other target compiler for both
x64 and ARM64 Windows executables.

```sh
bash tools/test_shared_futex_windows_component.sh \
  /path/to/llvm-mingw/bin/aarch64-w64-mingw32-clang++ shared-futex-component.exe
```

Run the executable on Windows with no arguments. Run it again with
`GESHER_MMAP_NO_PLACEHOLDERS=1` to exercise the other mmap implementation. The
fixture links production mmap, futex dispatch and Bionic pthread code. It uses
separately opened hard links, distinct virtual addresses, a nonzero file
offset, a read-only waker mapping, two competing waiters and a terminated
waiter. It exercises the registered Bionic mutex, condition variable and
reader-writer lock APIs over the same file-backed transport. The tested paths
call the Windows kernel. The fixture adapts descriptor admission and leaves
binder, seals, guest signal delivery, process cloning and the ELF translator
outside the component boundary.

The raw futex implementation at `299bfc990ae05eabc571dadf31de6e72606b1304`
passed the cross-process transport fixture on Windows 11 ARM64 in Parallels,
on both the native ARM64 target and the emulated x64 target with each mmap
implementation. The x64 fixture SHA-256 was
`20aa26c4da7fa7ef05eb75650dfb027e994fe9c25bc5ca5871c665b49232b1a7`
and the ARM64 fixture SHA-256 was
`d13300b3f701e5de3c1d3827a98a3255861e531aaa47224f6e1dbc7180ae9580`.
The Android ELF comparison at the candidate Windows revision still requires
a rebuilt Gesher runtime.

The fixture that also links the registered Bionic pthread implementation
passed on both Windows targets and both mmap implementations, with exit
status 0 in all four runs. Its x64 SHA-256 was
`04f8451cbfeb30b8a587abc6c529f9c4120bf08276c56a86266d97fd7eab071d`
and its ARM64 SHA-256 was
`f3741eaaf356d8051bdb79dc4c7ba0e96cd3eeb84721fd64a8b8c41b888380d9`.
